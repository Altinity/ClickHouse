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

- **B6 — behavior switch (1b+1c+1d) IN PROGRESS; resume state.** Severity: n/a (worklog/resume marker).
  Branch `cas-gc-part-manifest-impl`. The build does NOT link yet — expected mid-switch red (gate = green
  `Cas*:Ca*` sweep after 1d).

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

## Resolved

(none yet)
