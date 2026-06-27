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

  REMAINING (resume in order) — this is the 1d CORE, NOT yet started; build still RED:
  1. **1d T3-T8 — `CasGc.{h,cpp}` (the big one, ~2113 lines).** Still entirely on the snap/closure model:
     every method takes `std::map<uint64_t, GcSnap>&` (`fold`, `foldShardRecords`, `recheck`, `trim`,
     `cascadeAndPersist`, `assertSnapJournalCoherent`, `persistGenerationProbingUpward`, `loadSnap`).
     Rewrite `fold` over the single ordered `RootOwnerEvent` journal (owner-move ⇒ no delta/no cleanup;
     true removal ⇒ −1 + cleanup; activation ⇒ +1 subject to fold barrier #23 — don't advance the cursor
     past a live missing-body precommit; context-specific 404 = clamp+record, never wedge); add
     `foldManifestEdges`; wire `CasBlobInDegree` (EXISTS, 1d T2) + `CasGenerationSeal` (EXISTS, 1d T1:
     `CasFoldSeal` at fold / `CasCompletionSeal` at completion, resume keys off which seal exists); create
     `CasOrphanManifestSweep.{h,cpp}` (per-namespace, sealed owner view, `sweepEligible` from a durable
     watermark fact per OQ6, exact-token, no blob deltas); `CasGcFormats` `snap_*`→generation pointer;
     DELETE `CasGcSnap.*` (479), `CasTreeCodec.*` (170), `CasClosureWalk.*` (43) + their machinery
     (cascade, `children_by_tree`, expansion markers, resident snap); prune `FormatId::Tree`/`GcSnap` +
     magics in `CasFormat.{h,cpp}` (1d T1 left this enum prune pending). PRESERVE rev.15 invariants:
     exact-token delete only, global-then-shard fence, fold-through-fence recheck, `ViewableRound`,
     `deadTok`/no-return, never-GET-a-condemned-blob, GC never throws on a 404 during fold/sweep.
  2. **1d T9** `CasFsck` read-only manifest audit (OQ8: owner-visible missing body ⇒ error; reclaimable
     pre-precommit body ⇒ info; same sealed-owner-view + eligibility as the sweep). `CasFsck.cpp` (194)
     still tree-based.
  3. **1d T8 (tests)** rewrite/delete obsolete gtests (`gtest_cas_gc_snap.cpp`, `gtest_cas_tree_id.cpp`,
     `gtest_cas_closure_walk.cpp`, old `gtest_cas_build*.cpp`, `gtest_cas_gc_*`, `gtest_cas_store.cpp`,
     `gtest_cas_codecs.cpp` tree blocks, `gtest_cas_protocol_scenarios.cpp` tree_size cases,
     `gtest_cas_gc_leak.cpp` adopt-by-tree) + adapt `cas_test_helpers.h` (`publishRaw`→`RootRef`/
     `RootOwnerEvent`; add `writeManifestRaw`). Write the new TDD gtests the plans specify
     (`CasGcFold`, `CasOrphanManifestSweep`, `CasStore` readManifest, `CasBuild` promote fail-closed). NOTE
     the isolated 1b/1d gtests written in run 1 are still UNRUN (link blocked) and the read-path 1c gtests
     in the 1c plan (`CasStore.ResolveReturnsManifestId`, `.ReadManifestValidatesBodyAndFailsClosed`,
     `.LookupAndListOverManifestEntries`, `.ManifestCacheIsKeyedByIdAndToken`) are NOT yet written —
     `cas_test_helpers.h` lacks the `RootRef` `publishRaw` overload + `writeManifestRaw`.
  4. **Gate** — `unit_tests_dbms` links; `--gtest_filter='Cas*:Ca*'` green except known B3.

  WHY STOPPED HERE: 1c + 1b T6 + write-path are clean committed boundaries (each migrated file has zero
  non-comment references to the removed types — verified by grep). The 1d CasGc rewrite is a large,
  safety-critical, TLA+-grounded rewrite of ~2100 lines that must preserve INV_NO_DANGLE/NO_LOSS/NO_RETURN
  and the fold-barrier #23 semantics; it deserves its own focused run rather than a rushed pass. Files
  touching removed types now (verified by non-comment grep): only `Core/CasGc.{h,cpp}`,
  `Core/CasFsck.cpp`, `Core/CasGcSnap.*`, `Core/CasClosureWalk.*`, `Core/CasTreeCodec.*`, and the gtests.
  (`Core/CasBuild.cpp`, `PartPathParser.h`, `ContentAddressedWriteBuffers.h`,
  `ContentAddressedMetadataStorage.h` are CLEAN. `Core/CasPlacement.h` — the dead tree-era `visitPlacement`
  helper — was DELETED this run and its only include, in `CasBuild.cpp`, removed.)

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
