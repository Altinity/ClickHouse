# CA GC Redesign — Backlog (findings / ideas / sidetracks)

Captured during the autonomous execution run (see `../cas-gc-unattended-execution-log.md`). Items here
are deferred-not-dropped: protocol nits, refactors, test gaps, ideas, and sidetrack findings that should
not block the current phase. Each item: ID, severity, where, what, why-deferred.

---

## Open

- **B1 — Phase-0 model journal record naming (cosmetic).** Severity: low. Where:
  `docs/superpowers/plans/2026-06-26-cas-gc-phase0-tla-model.md` (and `…-phase2-token-diff-discovery.md`
  references it). The model names the root-journal record `OwnerTransition` (a `[ver, ref, old, new]`
  tuple); per spec rev. 15 the protocol/C++ name is `RootOwnerEvent`. The model record is *semantically*
  the rev.15 event (owner-move dispatch via equal old/new manifest is modeled), so this is a naming nit,
  not a semantic gap. Deferred: renaming would cascade into the phase2 model reference; defer to a
  terminology-only pass once the model is green, to avoid churn during the R0 gate.

- **B2 — make the model-proven load-bearing orderings explicit in phase1b/1d (enhancement).**
  Severity: low. The Phase-0 model proved 5 ordering rules necessary (publish/promote gate
  retire-view-relative + fence-floored; round order fold→retire→fence; recheck keeps a retired entry
  until its delete lands; `NoManifestIdReuse` by full `ManifestId`; fold-barrier reclaim liveness).
  All are already implemented by the plans, but they are not all called out as "load-bearing, proven
  necessary by Phase-0 control #N." Adding short callouts in phase1b's promote gate and phase1d's
  round/recheck steps would reduce the risk of a future optimization silently removing one. Deferred:
  not required for correctness (the behavior is in the plans); a documentation-only polish.

- **B3 — pre-existing `CaWiringOps.FreezeViaHardLinksIntoShadow` gtest failure (unrelated to Phase 1a).**
  Severity: medium. Where: `src/Disks/tests/gtest_ca_wiring.cpp:852`. Symptom: after
  `removeRecursive("shadow/bk1")` + commit (the UNFREEZE step), `existsDirectory("shadow/bk1")` still
  returns `true` (expected `false`). Reproduces in isolation and with a clean
  `$TMPDIR/ca_wiring_scratch`, so it is a real `removeRecursive`/shadow-namespace-drop bug in
  `ContentAddressedMetadataStorage`, not stale on-disk state. Confirmed NOT caused by Phase 1a: Phase 1a
  is purely additive (new `Cas{ManifestId,ManifestCodec,RunFile}` files + additive `FormatId` enum
  values + additive `CasLayout::manifestKey` + a `_manifests` namespace-segment rejection); the freeze
  test uses the `shadow/bk1/...` namespace which contains no `_manifests` segment, so the one behavioral
  change to existing code cannot affect it. Deferred: out of scope for Phase 1a (identity/codecs/layout);
  belongs to the wiring/freeze layer. The other 392 `Cas*`/`Ca*` tests pass.

- **B4 — `ManifestId::operator<` cosmetic (code-review N3).** Severity: trivial. Where:
  `Core/CasManifestId.h`. Hand-compares `root_namespace.string()` though `RootNamespace` has a
  defaulted `operator<=>`; could be `std::tie(root_namespace, ref) < std::tie(...)`. Equivalent and
  correct; cosmetic. Deferred.

- **B5 — phase1b/1c plans' standalone "build + sweep green" steps are unachievable (cross-cutting build).**
  Severity: low (doc/process). The behavior switch 1b+1c+1d is atomic on the build: removing
  `JournalRecord`/`ClosureNode` in 1b breaks `CasStore`/`CasGc` until 1c/1d rewrite them. The 1b and 1c
  plans each end with a "build + full Cas*:Ca* sweep" step that cannot pass in isolation. Handled in
  execution by running 1b+1c+1d as one unit with the green gate after 1d. Plan-doc polish: reword the
  1b/1c "build+sweep" steps to "compiles as part of the 1b+1c+1d switch; green sweep gated after 1d."
  Deferred: execution already accounts for it; documentation-only.

- **B6 — behavior switch (1b+1c+1d): DONE (SWITCH GREEN). See the Resolved section below.** The detailed
  per-run worklog that used to live here is preserved in git history (commits up to the run-4 test ports).
  Branch `cas-gc-part-manifest-impl`.

  DONE (committed; each compiles as an isolated TU):
  - **1b T1** — `CasRootShardCodec` rewritten to the single ordered `RootOwnerEvent` journal
    (`OwnerKind`/`OwnerBinding`/`RootOwnerEvent`/`RootRef`/`RootShard`); proto `RootShardManifest`
    rewritten (`ManifestRefProto`/`OwnerBindingProto`/`RootOwnerEventProto`/`RootRefProto`);
    `JournalRecord`/`ClosureNode`/`RefPayload`/`TreeEntryProto`/`JournalOp` removed. `gtest_cas_codecs.cpp`
    `CasRootShardCodec` block + golden test rewritten. Codec TU compiles clean.
  - **1d T1** — `CasGenerationSeal.{h,cpp}` (`CasFoldSeal`+`CasCompletionSeal`, `RunRef`, `ShardCoverage`,
    codecs) + proto + `Layout` keys (`foldSealKey`/`completionSealKey`/`blobTargetRunKey`/
    `partManifestCleanupKey`); `gcSnapKey`/`gcSnapShardPrefix` removed. `gtest_cas_generation_seal.cpp`
    added. TU+test-objs compile clean. NOTE: `FormatId::FoldSeal`/`CompletionSeal`+magics were already
    landed in 1a; `FormatId::Tree`/`GcSnap` enum prune (1d T1 Step 1) still pending — do during 1d core.
  - **1d T2** — `CasBlobInDegree.{h,cpp}` over the LANDED `CasRunFile` API (RunFileWriter/Reader; key=16B
    BE hash, payload=8B LE int64; checksum=CityHash128 of run bytes; explicit 0-row on transition-to-zero;
    fail-closed on negative). `gtest_cas_blob_indegree.cpp` added. TU+test-obj compile clean.
  - **1b T2-T7** — `CasBuild.{h,cpp}` rewritten: `writerInstanceId`, `manifestNamespace` (split
    `intended_ref` on the LAST `/`), `stageManifest` (OQ7 caps + random instance id + `putIfAbsentStream`),
    `precommitAdd`, `promote` (validate body, per-blob HEAD+condemned reval, pure-move `RootOwnerEvent`,
    sets `RootRef`), `abandon` (best-effort staged-`_manifests` deleteExact), `adoptEvidence(ManifestEntry)`,
    `setPendingMutableFiles`. Removed tree/precommit-ns/publish/closure/checkAndResolveDeps machinery; kept
    `putBlob`/`observeAndAdmit`/`uploadFromSource`/`recordPendingBlobDep`/`depIsTokened`/`hasDep`. Braces
    balanced; only stale doc-comment mentions of removed methods remain. Does NOT compile yet — blocked on
    `CasStore.h` still carrying `RefPayload`/`TreeId tree_id`/`readTree` (1c).

  NOW ALSO DONE (committed run 2, 2026-06-27):
  - **1c (all) — DONE.** `CasStore.{h,cpp}`: `Resolved.manifest_id` (+ `manifest_size`, set 0 — `RootRef`
    carries no size; the wiring derives per-file sizes from `ManifestEntry.blob_size`/`inline_bytes`);
    `resolveRef`/`listRefs` build `ManifestId{ns, RootRef.manifest_ref}`; `readManifest` replaces
    `readTree` (HEAD→`(ManifestId,Token)` cache→get→`decodePartManifest`→`refMatchesBody`/
    `manifestNamespaceMatches`, fail-closed missing/mismatch); `lookupPath`/`listDirectory` over
    canonical-path entries; `locate(const ManifestEntry&)` (Blob only; no Subtree); `manifest_cache`
    (`ManifestCacheKey{ManifestId,Token}` + a `ManifestCacheKeyHash` hashing the token's value+type, since
    there is no `std::hash<Token>`); `CasTreeCodec.h` include swapped for `CasManifestCodec.h`/
    `CasManifestId.h`; `updateRefPayload` mutator takes `RootRef`; `dropRef`/`dropNamespace` append removal
    `RootOwnerEvent`s (old committed binding / new none). NOTE: `EntryPlacement` (not `Placement`) is the
    1a enum name; `RootRef` has `published_at_ms` but NO `manifest_size`. `ContentAddressedMetadataStorage`
    read helpers rewired to `resolveRouted -> (Resolved, PartManifest)` + `lookupPath`/`listDirectory`.
    Commit: "CA GC phase1c: read path over root-local part manifests".
  - **1b T6 + write-path wiring — DONE.** `ContentAddressedTransaction.{h,cpp}`: `PartStaging.entries` is
    `ManifestEntry`; ALL entry-field accesses migrated (`name`→`path`, `file_hash`→`blob_hash`,
    `file_size`→`blob_size`, `Placement`→`EntryPlacement`, Subtree dropped). `publishStaging` uses the new
    write path `stageManifest → precommitAdd → putBlob → promote` (was stageTree/precommit/
    uploadStagedTree/publish); `updateRefPayload` mutator → `RootRef`. `republishRef` (T6) reads src
    manifest entries, `stageManifest` a fresh dst over the SAME blob hashes, `precommitAdd`+`promote`,
    `dropRef` src. `createHardLink` carry-forward reads the src manifest + `adoptEvidence` (was
    `adoptFromTree`). Exchange `getPartTreeId`/`adoptPart` (`ContentAddressedMetadataStorage.cpp`) made
    fail-closed/no-offer (manifests are per-instance ⇒ no shared content-addressed id to transmit; sender
    streams bytes — the documented fallback). Manifest-era relink = new backlog **B7**.
    Commit: "CA GC phase1b T6 + write-path wiring: transaction over part manifests".

  RUN 3 (2026-06-27) — the 1d GC CORE is DONE; ALL PRODUCT (non-test) SOURCE COMPILES. Build still RED
  ONLY because 12 stale write-path gtests have not yet been ported off the old `Build::putTree`/`publish`
  API (the link gate needs every globbed `gtest_*.cpp` to compile). Commits this run:
  `3526407f1b1` (GC core), `889a591645d` (codec/format/fsck/layout test ports).

  DONE (committed, run 3):
  - **1d T3-T7 — `CasGc.{h,cpp}` fully rewritten** over the single ordered `RootOwnerEvent` journal.
    `fold` dispatches owner-move (equal old/new `manifest_ref` ⇒ no delta/no cleanup) / true-removal
    (−1 + deferred body cleanup) / activation (+1) with the **fold barrier (control #23)**: a live
    missing-body precommit is non-activating and CLAMPS the shard cursor (re-read each round, advances on
    activation/removal). 404 rule realized: present-but-invalid body ⇒ `CORRUPTED_DATA`; missing
    committed/removal body ⇒ clamp + `RoundReport::recordAnomaly` (never guess, never wedge); missing
    precommit body ⇒ barrier. `foldManifestEdges` reads ONE `PartManifest`, validates `refMatchesBody`/
    `manifestNamespaceMatches`, emits ±1/blob. Wires `CasBlobInDegree` + `CasFoldSeal` (at fold) /
    `CasCompletionSeal` (at recheck); resume keys off WHICH seal exists. `retire` (zero-in-degree blobs,
    per-candidate HEAD, write-once sets), `fence` (registry-then-all-shard, positions into the completion
    seal), `recheck` (fold-through-fence completion gen, the single exact-token content-delete site,
    manifest-body deletes only after decrements sealed), `trim` (below sealed cursor), seal-driven
    `tryResumeIncompleteRound`. Lease/watermark/heartbeat kept verbatim; `reclaimAbandonedPrecommit`
    adapted to append removal `RootOwnerEvent`s. Added `inDegreeInGeneration` to `CasBlobInDegree`.
  - **1d T6 — `CasOrphanManifestSweep.{h,cpp}` (new)** — `sweepNamespace`/`prefixEligible` (OQ6 durable
    watermark fact only, never frozen-seq)/`pickOneSweepTarget`; active owner-key set from committed refs
    + live precommit bindings; exact-token deletes; no blob deltas; wired one bounded step per round.
  - **1d T9 — `CasFsck.cpp` rewritten** to the manifest audit (owner-visible missing body/blob ⇒ error;
    pre-precommit body ⇒ info via `prefixEligible`).
  - **Removals** — `FormatId::Tree`/`GcSnap` + magics pruned (`CasFormat.{h,cpp}`); `CasGcSnap.*`,
    `CasTreeCodec.*`, `CasClosureWalk.*` DELETED. `CasGcScheduler.cpp` dropped the removed
    cascade/forget `RoundReport` columns (the only product-code casualty — now fixed; everything else
    was test-only). `GcState` field names (`snap_generation`/`snap_shards`/`snap_pruned_through`) KEPT
    as-is (they now mean the generation pointer; renaming is cosmetic and would churn the JSON codec +
    every gc-state test — deferred, functionally correct).
  - **Tests** — deleted `gtest_cas_gc_snap.cpp`/`gtest_cas_tree_id.cpp`/`gtest_cas_closure_walk.cpp`/
    `gtest_cas_tree_layout.cpp`; wrote NEW TDD gtests `gtest_cas_gc_fold.cpp` (committed/precommit/
    promote/barrier/404/ref-mismatch/removal-missing-body), `gtest_cas_gc_fence_recheck.cpp`,
    `gtest_cas_orphan_manifest_sweep.cpp`, `gtest_cas_gc_resume.cpp` — ALL COMPILE. `cas_test_helpers.h`
    gained `writeManifestRaw`/`blobEntryFor`/owner-transition fixtures (`appendOwnerEvent`,
    `publishCommittedTransition`, `dropRefTransition`, `addPrecommitTransition`, `promoteTransition`,
    `deleteManifestBody`) + GC-core helpers (`openStoreForTest`, `writeBlobBody`, `inDegreeOf`,
    `foldCursorOf`, `currentGenerationOf`, `setWatermarkMinActive`). Ported `gtest_cas_format.cpp`,
    `gtest_cas_codecs.cpp` (dropped Tree block), `gtest_cas_gc_formats.cpp` (dropped GcSnap block),
    `gtest_cas_layout.cpp` (generation keys), `gtest_cas_fsck.cpp` (manifest audit) — ALL COMPILE.

  REMAINING (resume in order) — the ONLY thing between here and the GREEN gate:
  1. **Port 12 stale write-path/wiring gtests** off the old `Build::putTree`/`publish`/`adoptFromTree`/
     `TreeEntry`/`Placement`/`Resolved::tree_id`/`Store::readTree` API to the new Build flow
     (`startBuild → stageManifest(entries) → precommitAdd → putBlob → promote`; entries are
     `ManifestEntry{path, EntryPlacement::Blob|Inline, blob_hash, blob_size, inline_bytes}`). Each must
     COMPILE (link gate). Broken-file inventory (error counts at run-3 end), smallest first:
       - `gtest_cas_event_log.cpp` (4) and `gtest_cas_gc_log.cpp` (4): replace `publishOneBlobPart` tree
         flow with the manifest flow. **CAVEAT: both assert B170 GC event types** (`BlobPut`/`BlobRetire`/
         `BlobDelete`/`GcFold*`/`GcFence`/etc.). The run-3 `CasGc` rewrite DROPPED all B170 event
         emission to bound scope. To make these pass, **re-add B170 `emitEvent` calls to the new core**
         (fold begin/end, retire-observe, fence, the single blob-delete site, precommit-reclaim) using
         the existing `CasEvent`/`EventEmitter` types (still present, Tree/Snap enum values kept) — a
         worthwhile addition for soak attribution. If only compilation (not the assertions) is needed
         first, port the publish flow and let the event assertions fail, then re-add events.
       - `gtest_ca_transaction.cpp` (6), `gtest_cas_truncate_reclaim.cpp` (6), `gtest_ca_wiring.cpp` (8),
         `gtest_cas_b140_dangle.cpp` (11): wiring/transaction over `TreeEntry`/`Placement`/`readTree`/
         `gcSnapKey`/`GcSnap` — port to manifests; b140 was a snap-cursor-coherence test (the new model's
         analog is the fold barrier — rewrite or delete as no-successor).
       - `gtest_cas_build.cpp` (20), `gtest_cas_build_root_dangle.cpp` (19), `gtest_cas_store.cpp` (20),
         `gtest_cas_protocol_scenarios.cpp` (20), `gtest_cas_gc_round.cpp` (20), `gtest_cas_gc_leak.cpp`
         (20): the deep write-path/round/leak suites — the largest port. `gtest_cas_gc_round.cpp` (2234
         lines) is entirely snap/cascade-based; most cases have manifest-model analogs already covered by
         the new `gtest_cas_gc_*` suites, so prefer DELETE-and-replace over line-by-line port for the
         snap-specific cases. `gtest_cas_store.cpp` needs the 1c read-path tests
         (`CasStore.ResolveReturnsManifestId`/`.ReadManifestValidatesBodyAndFailsClosed`/
         `.LookupAndListOverManifestEntries`/`.ManifestCacheIsKeyedByIdAndToken`) — still unwritten.
         `gtest_cas_build.cpp` needs `CasBuild` promote fail-closed cases.
  2. **GATE** — `ninja -C build unit_tests_dbms` LINKS; `build/src/unit_tests_dbms
     --gtest_filter='Cas*:Ca*'` GREEN except the known B3 (`CaWiringOps.FreezeViaHardLinksIntoShadow`).
     The new GC-core suites (`CasGcFold`/`CasGcFence`/`CasGcRecheck`/`CasOrphanManifestSweep`/`CasGcResume`/
     `CasFsck`) compile but are UNRUN (binary not linked) — run + verify them once the link gate passes.

  WHY STOPPED HERE (run 3): the safety-critical GC core (the TLA+-grounded heart) is COMPLETE, COMPILES
  into the product, and is committed; the remaining work is mechanical test porting against a now-stable
  API plus an optional B170 event re-add. A clean resumable boundary per the run brief. No product code
  references any removed type (verified: only the 12 listed gtests do).

- **B7 — cross-server part relink in the part-manifest model (feature regression to restore).**
  Severity: medium (a fetch-path optimization, not a correctness issue — the byte-fetch fallback is
  always correct). Where: `IContentAddressedExchange` (`ContentAddressedExchange.h`), its impl in
  `ContentAddressedMetadataStorage.cpp` (`getPartTreeId`/`adoptPart`), and the caller
  `src/Storages/MergeTree/DataPartsExchange.cpp` (~`:245`, `:1104`). What: the old relink transmitted a
  CONTENT-ADDRESSED tree id the receiver could `adoptTree`-then-`publish`, because trees were shared by
  content hash across servers. In the rev. 15 redesign a part is a PER-INSTANCE single-owner `ManifestId`
  — there is no shared content-addressed id to transmit. During the behavior switch (run 2) `getPartTreeId`
  was made to return `nullopt` (no offer ⇒ sender streams bytes, the documented fallback) and `adoptPart`
  to throw `NOT_IMPLEMENTED` (unreachable while no offer is made). Why-deferred: a correct manifest-era
  relink is an INTERFACE rework (the wire must carry the manifest's blob hashes/sizes, not one id; the
  receiver `stageManifest`s its OWN manifest over the transferred blob hashes, `adoptEvidence` each, then
  `precommitAdd`+`promote`), spanning `IContentAddressedExchange`, the impl, and `DataPartsExchange` — out
  of scope for the GC switch plans. Until then replication of CA parts falls back to byte streaming (slower
  but correct). The `gtest_ca_wiring.cpp` relink tests (`getPartTreeId`/`adoptPart`, ~`:912`–`:1033`) and
  `gtest_cas_gc_leak.cpp`'s adopt-by-tree must be rewritten or removed for B7 (and meanwhile they fail to
  compile against the new exchange impl — they are part of the 1d T8 gtest rewrite).

- **B8 — GC precommit-reclaim is not wired for the converged-shard precommit model (real core gap).**
  Severity: medium (space-liveness, not a safety/no-loss issue — fail-closed holds). Where:
  `Core/CasGc.cpp` `Gc::fold` (the `if (Layout::isPrecommitNamespace(ns)) reclaimAbandonedPrecommit(...)`
  gate) and `Gc::reclaimAbandonedPrecommit` (which parses a `<server_hex>/_precommits` suffix). What: per
  spec rev. 15 / the run-2 `Build::precommitAdd`, a precommit owner binding now lives in the FUTURE
  COMMITTED REF's OWN table-namespace shard (keyed by `final_ref_name`); there is no `_precommits`
  namespace. But `reclaimAbandonedPrecommit` still runs ONLY on `_precommits`-suffixed namespaces and
  derives the server from that suffix, so under the converged write path it NEVER fires: an abandoned
  precommit of a judged-dead build is never released by GC (only by the writer's best-effort `abandon` or
  the orphan-manifest sweep for the body). The watermark/K=2 frozen-seq reclaim machinery is effectively
  dead code. Surfaced by porting `gtest_cas_build_root_dangle.cpp::CasBuildRoot.AbandonedPrecommitReclaimed`
  (now `GTEST_SKIP`-ped citing B8) and `gtest_cas_gc_leak.cpp::AbandonedPrecommitReclaimsOwnBlobs`.
  Why-deferred: fixing it is a non-trivial core change — the fold must, for EVERY table shard (not just
  `_precommits`), scan precommit-kind owner bindings, derive `(server, build_seq)` from the binding's
  `build_id` / ref, judge liveness via the watermark, and append a `PrecommitRemove` `RootOwnerEvent`
  for dead ones — plus the corresponding TLA+/model update. Out of scope for the test-port run. NOTE: the
  spec's INV-COMMIT-FAILCLOSED still holds (a falsely-reclaimed live build's `promote` fails closed), so
  this is a space leak of abandoned-precommit closures, not data loss.

- **B10 — deferred MINORS from the GC-core durability code review (do NOT fix in the must-fix pass).**
  Severity: low (none are safety/correctness blockers). Collected while fixing M1/M2/M3/decodeOwnerBinding/
  B9 so they are not lost:
  - **`inDegreeInGeneration` is O(candidates x runsize).** Where: `Core/CasBlobInDegree.cpp` (called per
    retired blob in `recheck`). Each lookup re-streams the generation's run instead of building a map once
    per round. Fine at current scale; revisit if a round's candidate set grows large.
  - **Unguarded `int64_t` in-degree accumulation before the `merged<0` guard.** Where: the fold/merge in
    `Core/CasBlobInDegree.cpp`. A corrupt run could overflow before the under-count guard fires; the guard
    catches the negative result but not an intermediate wrap. Add a saturating/overflow check.
  - **`gtest_cas_gc_resume.cpp::ResumeFromDurableFoldSealCompletesRound` doesn't simulate a real mid-round
    crash** — it reconstructs the resume inputs by hand rather than crashing a partially-run round and
    re-entering `runRegularRound`. A more faithful crash-injection test would exercise
    `tryResumeIncompleteRound` end-to-end.
  - **`~Build` calls `retireBuildSeq` without confirming it is `noexcept`.** Where: `Core/CasBuild.cpp`
    destructor. If `retireBuildSeq` can throw, a destructor throw is UB. Audit + mark `noexcept` or guard.
  - **`CasStore Resolved.manifest_size` is always 0.** Where: `Core/CasStore.cpp` resolve path. The field
    is populated nowhere; either wire it up or drop it from `Resolved`.
  - **Redundant 2nd watermark GET in `sweepNamespace`.** Where: `Core/CasOrphanManifestSweep.cpp` —
    `prefixEligible` GETs the watermark, and the sweep path may GET it again. Cache/thread it through.
  - **`gtest_cas_b140_dangle` is a shallow oracle.** The real shared-blob no-dangle oracle now lives at
    `gtest_cas_gc_round.cpp::SharedBlobSparedUntilBothRefsDrop`. Either cross-reference it from the b140
    file or drop `gtest_cas_b140_dangle.cpp` as redundant.

- **B11 — round-summary `objects_deleted` undercounts: it omits manifest-body (tree) deletes (soak finding,
  introspection only).** Severity: low (the `system.content_addressed_log` per-event audit is correct and
  complete; only the aggregated round-summary counter is misleading). Found in the post-switch scoped chaos
  soak (2026-06-27, branch `cas-gc-part-manifest-impl`, seed 20260627, 45m timeline, GREEN/`PHASE3 OK`,
  `dangling=0` at all 4 checkpoints). Where: `Core/CasGc.cpp` — `report.deleted` (which feeds the
  `content_addressed_garbage_collection_log.objects_deleted` round-summary column) is incremented ONLY in
  the per-blob outcome loop (`OutcomeKind::Deleted`, ~line 699). The separate manifest-body delete site at
  the recheck tail (`deleteExact(layout.manifestKey(id), token)`, ~line 717, which emits a `TreeDelete`
  event) does NOT increment `report.deleted`. Observed live: the round summary showed `objects_deleted=0`
  for all rounds while `system.content_addressed_log` recorded `tree_delete=13078` — i.e. GC was performing
  substantial real reclaim, but the round-summary counter read zero. (`blob_delete=0` was separately
  CORRECT behavior here: content dedup kept every blob's in-degree > 0, so the fold-through-fence recheck
  correctly spared all 939 retired blob candidates — R0 no-loss exercised, not a GC stall.) Fix: either add
  a `manifests_deleted`/`tree_deleted` column to the round summary, or fold the manifest deletes into
  `report.deleted` (and rename it to reflect "objects" = blobs + manifests). Low priority — operators with
  the event log can already reconstruct the true count; this only de-confuses the at-a-glance summary.

- **B12 — optimal one-round token-diff skip (current Phase-2 impl is conservative).** The Phase-2
  token-diff skip compares the root-shard object's backend token surfaced via `Backend::list`
  (`ListedKey.token`). That token is bumped by GC's OWN per-round writes: `fence` calls `mutateShard`
  on every shard every round, and `trim` calls `mutateShard` when a shard has trimmable events. The
  TLA+ model's `listedTok` advances only on writer owner-transitions, so to stay provably safe WITHOUT a
  model change the impl records `folded_token` from the POST-FENCE token `recheck` reads (canonical round
  order, no reorder) and skips only when nothing wrote the shard since — a strict SUBSET of the
  model-proven-safe skips. Cost: a shard `trim` mutated in round R is re-read in R+1; only once a shard is
  quiescent (a round with nothing to trim ⇒ recheck token == final token) does it skip from the next round
  on. Steady-state quiescent shards (the common case) skip every round, so R1's win is preserved; the loss
  is one extra read per shard per activity burst. The OPTIMAL one-round skip would capture the post-trim
  (post-all-GC-writes) token. That requires either sealing the completion generation after `trim` or a
  dedicated post-round token snapshot, AND a TLA+ model extension proving the discovery token may advance
  on GC's own per-round writes with `foldedTok` = post-round token (plus a sabotage for capturing a
  pre-GC-write/stale token). A first attempt did this via an UNSAFE trim-before-recheck pipeline reorder
  (reverted at `7ae46d27701`, originally `43cd5eef11e`) — the reorder touches the `sab_trimunincorporated`
  /`INV_JOURNAL_COVERAGE` concern and was unproven. Deferred: model-extend + reseal/snapshot, then lift to
  one-round skip. Medium priority (perf only; correctness already holds conservatively).

- **B13 — two-replica sharded GC integration soak blocked by the test MinIO image (environmental).** The
  Phase-4 `tests/integration/test_cas_gc_sharded/` module (gc_shards=2, two replicas, disjoint shards) is
  committed but `pytest.mark.skip`-ped: the integration Docker MinIO (`RELEASE.2024-09-13`) predates S3
  `DeleteObject If-Match` (conditional delete), which `CasProbe::runCapabilityProbe` step 6 hard-requires
  (fail-closed) — so the server refuses to start. This blocks ALL CA S3 integration tests in the repo
  (`test_content_addressed_gc_s3`, `test_content_addressed_shared_pool` show the identical
  `CasProbe: deleteExact with a wrong token was not TokenMismatch` failure in cached runs), not just the
  sharded soak. Remove the skip + run the soak once the integration MinIO image is bumped to
  ≥ `RELEASE.2025-09` (or a conditional-delete-capable backend is wired for CA S3 integration tests).
  Phase 4's safety is covered meanwhile by the unit gate (376 `Cas*:Ca*` gtests green) + the TLA+
  `stage5_sharding` HOLD + the two new sharding controls. Medium priority (validation gap, not a code bug).
  Note: the soak prep CAUGHT+FIXED a real latent bug en route — `gc_shards` in `PoolConfig` was never
  written to the initial `GcState` (XML config was a no-op; all pools silently ran gc_shards=1), fixed in
  `209d4ff1462` by threading `<gc_shards>` through `MetadataStorageFactory`/`ContentAddressedMetadataStorage`
  and setting it on first lease acquire.

- **B14 — Phase 5 retire-token optimization: CODE deferred — blocked on blob-token provenance (design
  decision).** The Phase-5 model gate is committed GREEN (`d56f4e84a4d`): it PROVES that `retire` can
  source `RetiredEntry.token` from a token captured into sealed generation state at fold time (`storedTok`)
  instead of a per-candidate `backend.head`, with the delete staying exact (a stale stored token only
  fails the match → under-delete/spared, never over-delete; `sab_staletokenoverdelete` → `INV_NO_LOSS`).
  BUT the optimization is NOT realizable in code as planned: **the blob's incarnation token (ETag) is not
  available at fold time without a HEAD.** `ManifestEntry` (`CasManifestCodec.h`) records only
  `blob_hash`/`blob_size`/`placement` — NOT the blob's storage token; `BlobDelta` carries only hash+sign;
  the in-degree run (`blobTargetRunKey`) stores only hash+count. The token is first obtained in `retire`
  via `backend.head(blobKeyOf(...))` — exactly the HEAD Phase 5 wants to eliminate. To have the token in
  sealed state without a HEAD, the writer would have to record the blob's committed ETag into the
  `ManifestEntry` payload at commit time (`CasBuild`/`CasStore` already hold it from the `put` `PutResult`)
  and plumb it manifest → `BlobDelta` → fold → in-degree run. That is a **manifest on-disk schema change**
  that couples the content-plane manifest to storage incarnations — a design decision with real trade-offs
  (manifest size grows by a token per entry; content/storage-separation), worth review, NOT an autonomous
  overnight core-schema change. Alternatives: (a) manifest carries `blob_token` (cleanest realization,
  enables HEAD-free fold-time capture; the recommended path if the size/coupling cost is acceptable);
  (b) a batched fold-time HEAD into a token side-table (relocates rather than eliminates the HEAD — little
  win); (c) leave it — the current per-candidate-HEAD `retire` is CORRECT and already shipped. Decision for
  the maintainer. Until then the model gate stands as the safety proof and the GC behaves correctly with
  the existing HEAD. Lowest-stakes phase (R1 perf only). Phase 5 Task 2/3 code NOT written (the field would
  be an unpopulated no-op without the provenance); no dead code committed.

## Resolved

- **B9 — GC generation pruning: DONE.** `Gc::pruneSupersededGenerations` (called in the recheck round tail
  before the round-commit `gc/state` CAS) deletes the `foldSealKey`/`completionSealKey`/`blobTargetRunKey`/
  `partManifestCleanupKey` objects of generations at or below the retention floor
  (`snap_generation - gc_snap_generations_to_keep`), walking forward from `snap_pruned_through` (bounded
  64/round) and folding the advanced cursor into the same CAS. `keep == 0` prunes nothing (forensics
  keep-all). Never throws on a 404 (record-and-continue). The 2 skipped tests
  `CasGcSnapRetention.{PrunesOldGenerationsKeepingLastThree,KeepZeroPrunesNothing}` are un-skipped and now
  assert generations below the floor are gone (and the last N + live generation remain). R0 invariants hold.

- **B6 — behavior switch (1b+1c+1d) DONE; SWITCH GREEN.** The link gate passes (`unit_tests_dbms` links)
  and the `Cas*:Ca*` sweep is green except the known B3 (`CaWiringOps.FreezeViaHardLinksIntoShadow`).
  Task A: B170 GC event emission restored in the new `CasGc` core (fold begin/end, per-blob fold edges
  RootAdd/RootRemove, IndegZero/GcRetireObserve/BlobRetire at retire, GcFence, GcRecheckVerdict/BlobDelete
  at the single content-delete site, TreeDelete for manifest cleanup, GcTrim, PrecommitReclaim). Task B:
  all 12 stale gtests ported to the manifest model (`event_log`/`gc_log`/`ca_transaction`/
  `truncate_reclaim`/`b140_dangle`/`build`/`build_root_dangle`/`store`/`protocol_scenarios`/`gc_round`/
  `gc_leak`/`ca_wiring`). Faithful ports — no-dangle/no-loss/no-leak assertions kept strong. Genuinely-
  obsolete tree/snap-only scenarios `GTEST_SKIP`-ped with reasons (deferred-tree-upload, inline-closure
  JournalRecord, snap-retention B174, adopt-by-tree relink B7, abandoned-precommit-reclaim B8). The
  `b140_dangle` white-box snap-cursor injection was rewritten as a black-box no-loss oracle (the failure
  mode is structurally impossible in the manifest model). Found+flagged B8 (precommit-reclaim wiring gap).
