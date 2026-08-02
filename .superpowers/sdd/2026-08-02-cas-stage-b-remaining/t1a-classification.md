# T1a classification — `CasRefCatalog::read` sites in `CasRefLedger.cpp`

Read-only gate for plan task T1a of `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`.
Derivation command and commit:

```
grep -n "CasRefCatalog::read" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
# at cas-gc-rebuild, HEAD = ce312f547c3
```

Ten sites found: lines 582, 600, 1221, 3217, 3247, 4699, 4717, 4769, 4845, 4903. Every enclosing
function was read in full.

## Classification table

Classes: 1 = admission/identity resolution (one snapshot per operation), 2 = mutation authority
(KEEP), 3 = current-life destructive per-key revalidation (KEEP), 4 = held-handle hot path
(REMOVE), 5 = duplicate read within one operation (REMOVE).

| # | line / enclosing function | operation in flight | class | disposition | reason |
|---|---|---|---|---|---|
| 1 | 582 `acquireReadableRefTableRuntime` (cold arm) | cold reader's first-touch admission of a resident ref-table runtime | 1 | KEEP | First catalog cut resolving the current `Live` life for a namespace this process has no runtime for. The warm held-handle path (lines 564–578) returns before this point and pays zero catalog GETs — the function's own comment states the hot-reader contract explicitly. |
| 2 | 600 same function, confirm read | same cold admission — token + decoded-value stability confirm immediately before publishing the runtime | 1 | KEEP | Not a duplicate: the comparison result is load-bearing (`catalog_changed` → `throwCasWriteRetryLater`). It guarantees a *fresh* resolution never installs a life that was superseded between the first cut and runtime publication. See "site 2 discussion" below for the failure asymmetry. |
| 3 | 1221 `resolveNamespaceLife` (retry loop) | mutation-side identity resolution / namespace birth | 1 | KEEP | One read per loop attempt, and every repeat follows a catalog *mutation* attempt (`createNamespace`, `completeCreation`, `reconcileStaleCreator`) whose outcome must be re-observed. Bounded by `kMaxResolveAttempts = 32`. Within any single successful resolution the life comes from exactly one snapshot. |
| 4 | 3217 `commitRefChunk`, `removal_append` arm | terminal removal-class ref-log append | 2 | KEEP | Re-read "immediately before id allocation" requiring the exact catalog life to still be `Removing`; a stale exact runtime must not append a terminal after the catalog life changed. Mandatory mutation authority. |
| 5 | 3247 `commitRefChunk`, `positive_append` arm | positive ref-log append (publish / set-published-at) | 2 | KEEP | "Final catalog admission observation before id allocation": closes the cached-runtime window in which another actor publishes `Removing` after the writer's ordinary entry gates. A non-`Live` exact row permanently closes the local positive lane. |
| 6 | 4699 `namespaceLife`, `removal_closed` arm | mutation identity resolution against a runtime whose positive lane is closed | 1 | KEEP | `reconcileCatalogCut(CasRefCatalog::read(...))` detaches a dead predecessor (lost-erase-response case) before refusing, so an absent/replaced row frees the logical name. Exceptional path only (`removal_admission_closed` set); never on a read path. |
| 7 | 4717 `namespaceLife`, cold arm | cold mutation observes or births the durable identity | 1 | KEEP | One snapshot per cold mutation. `Live` fast-paths from this snapshot; otherwise control hands off to `resolveNamespaceLife` (site 3), and the life is resolved exactly once either way — no two coexisting resolutions from different snapshots. |
| 8 | 4769 `dropNamespaceImpl`, initial read | drop admission / identity resolution | 1 | KEEP | Resolves the observed life for the drop, and is the seam where `dropNamespace(const NamespaceLifeId &)`'s `expected_incarnation` guard fires (lines 4773–4777): a stale held life gets `throwCasWriteRetryLater`, never a write against the successor. |
| 9 | 4845 `dropNamespaceImpl`, pre-`beginRemoving` | `Live -> Removing` transition authority after lane drain | 2 | KEEP | Fresh exact-row observation after an *unbounded* drain window (queue `cv.wait`); feeds the `beginRemoving` CAS. Incarnation mismatch against the recovered runtime throws. Not a class-5 duplicate of site 8: the second read gates the mutation, and equality is checked, not assumed. |
| 10 | 4903 `dropNamespaceImpl`, catch block | disambiguation of an ambiguous `beginRemoving` failure | 2 | KEEP | Fail-close resolution: `Removing` under the same life = conclusive success; lane reopens only after a fresh exact observation proves the *identical* original `Live` row and the same mount fence; every other case stays closed and rethrows. |

**Result: zero class-4 sites and zero class-5 sites.** No removal candidates exist in this file.

## Site 2 discussion (the only arguable classification)

The taxonomy tempts class 5 ("duplicate read within one operation"), but the second read is a
*confirm* whose inequality outcome aborts the admission. Failure asymmetry:

- KEEP costs one extra catalog GET per **cold** admission — once per namespace per process, never
  per read.
- REMOVE opens a window where a *fresh* resolution publishes a runtime for a life that another
  actor superseded (drop + rebirth) between the first cut and publication. That violates the
  contract's positive control ("a fresh resolution yields life2's value") — a stale-read through a
  handle the caller believes is *current*, until `invalidateRemovedCatalogLife` reconciliation
  happens to catch it.

The safe direction is KEEP; the cost direction is bounded and cold-path-only. The unused test hook
`readable_catalog_after_observation_hook_for_test` (`CasRefLedger.h:397,944`; fired at
`CasRefLedger.cpp:591`) sits exactly between the two reads and currently has **zero test users**
(tree-wide grep) — a natural injection point if the controller wants this race pinned.

## (a) Which sites do live table readers hit?

The read/list entry points are `resolveRef` (line 276), `listRefs` (line 350),
`hasAnyRefWithPrefix` (line 383) and `namespaceFilesLifeIfReadable` (line 4736). All four go
through `acquireReadableRefTableRuntime`, whose **warm path returns the resident runtime before
any catalog read** — a live table reader with a held runtime issues zero catalog GETs today.

- Sites 1–2 are the **cold** first-touch of that same function: a reader *does* reach them, but
  exactly once per namespace per process (or after invalidation), and that is admission, not the
  hot path.
- Sites 3, 6, 7 are mutation/birth identity (`namespaceLife` is reached only from mutation-side
  callers: `acquireMutableRefTableRuntime` line 616 and test-only accessors).
- Sites 4–5 are flush-leader commit authority; sites 8–10 are drop/removal.

**Prediction for T1a Step 3:** `HotRefReadThroughHeldLifeIssuesZeroCatalogRequests` (resolve once,
reset journal, then `listRefs` + point reads) should **pass immediately**, because the warm path
already pays no catalog GET. Per the plan's Step 3 rule, that means STOP, record it as a coverage
pin, and do not write production code for it. If it *fails*, the GETs are coming from somewhere
other than these ten sites (e.g. `readCkpt`/snapshot keys mis-attributed to the catalog key in
the journal filter) and the classification must be revisited — the test asserts specifically
against `layout.refCatalogKey()`, so a failure would name the offending key.

## (b) Held ref-WRITER seam

The ledger's mutation surface is name-keyed (`RootNamespace`), and every name-keyed writer
resolves the **current** life itself via `namespaceLife`/`acquireMutableRefTableRuntime` — no
caller-held life flows into it, so no stale-write-across-rebirth is representable there.

Exactly one life-taking mutation entry exists: `dropNamespace(const NamespaceLifeId &)`
(`CasRefLedger.h:170`) → `dropNamespaceImpl(ns, expected_incarnation)`. Its guard at
`CasRefLedger.cpp:4773-4777` refuses when the held incarnation differs from the current catalog
life — a stale holder can only be refused (or drop its *own* old life if the row still matches);
it can never target the successor. Callers: `CasDecommission.cpp:188,194`, plus
`Pool::dropNamespace` forwarding.

**Conclusion:** the stale-ref-writer requirement is structurally satisfied at the one held-life
writer seam that exists, and vacuous everywhere else. Do **not** invent a held ref-writer API.
One honest gap: no test pins the "exact removal life differs from current catalog life" refusal
(tree-wide grep of the message finds only the production site). That is a cheap optional pin, not
a T1a obligation.

## (c) REMOVE-site regression guards

Vacuous — the classification produced no REMOVE sites. For completeness, if the controller
overrides site 2 to class 5: the removal would be noticed by nothing today (that is the point of
the unused hook), which is itself an argument against removing it without first writing the race
test the hook was built for.

Cross-check requested by the dispatch: the `CasRefGcCleanupAuthority` tests
(`src/Disks/tests/gtest_cas_ref_gc.cpp:494-551`, four tests: catalog-token move before first
delete / between keys, GC-fence move before first delete / between keys) pin **class-3 per-key
`deleteExact` revalidation**, and those reads live in the GC/cleanup layer, **not** in
`CasRefLedger.cpp` — no site in this file is class 3, and no KEEP here conflicts with those
tests' expectations. Sites 4 and 9 are the ledger-side *mutation-authority* analogues and stay.

## (d) Open questions

None that block classification. Site 2 is the only site where the taxonomy is arguable (1 vs 5);
the resolution above is a KEEP either way, so no production edit hinges on it. Two observations
for the deferred-docs/backlog stream, not fix rounds:

1. `readable_catalog_after_observation_hook_for_test` is an unused test hook (no test users).
2. The `dropNamespaceImpl` exact-incarnation refusal is not test-pinned.

## Limits of this classification

It covers exactly the ten `CasRefCatalog::read` call sites in `CasRefLedger.cpp` at
`ce312f547c3`. It says nothing about catalog reads in `CasPool.cpp`, `CasGc.cpp`,
`CasNamespaceJanitor.cpp`, `CatalogLifecycleReconciler.cpp`, `CasOrphanManifestSweep.cpp`,
`CasFsck.cpp`, or `CasDecommission.cpp`, nor about non-catalog reads (`readCkpt`, snapshot/log
GETs) on the read path. The zero-GET prediction in (a) is a prediction, not a measurement — the
T1a Step 3 run is the experiment that confirms or refutes it.
