# T1a slice report — ref-side read contract: `CasRefReadContract`

Slice T1a, plan task T1 (`docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`, `{#t1a}`).
Consumes the classification gate
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t1a-classification.md`, which found all ten
`CasRefCatalog::read` call sites in `CasRefLedger.cpp` KEEP (zero class-4/5 removal candidates).
**This slice makes zero production edits** — the three tests below are coverage pins for a
contract the classification predicted already holds.

New file: `src/Disks/tests/gtest_cas_ref_read_contract.cpp`, suite `CasRefReadContract`, three
tests.

## Per-test record

### 1. `HeldRuntimeAfterSameNameRebirthReadsStaleOrNotFoundNeverSuccessorRefs`

Pins the read-side alias contract for committed refs. A ref is published under life 1 through the
real production write path (`beginPartWrite`/`stageManifest`/`precommitAdd`/`promote`); one
`resolveRef` call is made to hold the reader runtime resident. The namespace is then dropped and
re-admitted under the same logical `RootNamespace` (life 2), using the same lifecycle-real
sequence as `gtest_cas_ns_file_read_contract.cpp`'s `deleteCatalogLife`/`admitReplacementLife`
(`casUpdate` to `Removing`, `deleteCompletedRemoving` under a held fence, `casAdmitEntry` for the
successor) — not a raw sentinel overwrite. A different ref value is published at the same ref
name under life 2 via the raw ref-log fixture writer (`publishCommittedTransition`), since a
mutation issued through the SAME store's own ledger would resolve against its cached life-1
runtime, not life 2 — using the store's own write surface here would test the wrong thing.

Asserted: the held runtime's subsequent `resolveRef` call never returns life 2's `ManifestId`
(`EXPECT_NE` against it explicitly) and, when it resolves at all, returns life 1's value. The test
then forces the cached runtime out — touching a different namespace under a 1-byte
`ref_table_cache_bytes` budget, the same production whole-table-eviction knob
`RefTableCacheEviction.WholeTableEvictionUnderBudgetReRecovers` uses — and shows a positive
control: the SAME `Pool`, now cold for `ns`, re-recovers and resolves to life 2's value. This
avoids opening a second concurrent mount for the "fresh resolution" check; the eviction path is
itself a real production re-recovery, not a test-only shortcut.

Result: **PASS on the first run** (coverage pin, production already satisfied the contract).

Sensitivity check: temporarily changed the held-runtime assertion to `EXPECT_EQ(..., life2_manifest)`
(expect the successor's value). Failed as expected:
```
src/Disks/tests/gtest_cas_ref_read_contract.cpp:150: Failure
Expected equality of these values:
  held_after.has_value() ? std::optional<ManifestId>(held_after->manifest_id) : std::nullopt
  std::optional<ManifestId>(life2_manifest)
[  FAILED  ] CasRefReadContract.HeldRuntimeAfterSameNameRebirthReadsStaleOrNotFoundNeverSuccessorRefs (0 ms)
```
Full output preserved at `build/t1a_sensitivity_test1.log`. Mutation reverted; the real assertion
(`EXPECT_NE`) restored and re-verified green in the final run.

### 2. `HotRefReadsThroughHeldRuntimeIssueZeroCatalogRequests`

Pins the zero-catalog-GET hot path. A ref is published and read once (`CountingBackend`), then the
counters are reset and `resolveRef`/`listRefs`/`hasAnyRefWithPrefix` are driven again. Asserted:
every counter against `layout.refCatalogKey()` (head/get/casPut/put/putOverwrite) is zero — plus
the stronger claim that `touchedKeys()` is empty entirely, since `ensureRefTableRecovered` returns
immediately once `rt.recovered` is true (a warm ref read is a pure in-memory map lookup, not merely
catalog-free). Positive control, captured BEFORE the reset: the cold admission reached the catalog
(head/get/casPut counts on `refCatalogKey()` summed > 0) and read this namespace's own checkpoint
object (`getCount(layout.refCkptKey(life))` > 0) — the checkpoint GET, not a stream LIST, is what
`ensureRefTableRecovered`'s cold arm actually performs (`readCkpt` → one `backend.get`); an earlier
draft of this positive control asserted a nonzero `listCount` on the namespace stream prefix and
genuinely failed (recovery here uses the snapshot+checkpoint pointer, not a raw LIST), which is
the reason the checkpoint-GET form was used instead — see the "first-draft failure" note below.

Result: **PASS** once the positive control was corrected to the checkpoint-GET form.

Sensitivity check: temporarily changed the catalog-head assertion to `EXPECT_GT(headCount(refCatalogKey()), 0u)`
(expect a nonzero catalog HEAD on the warm path). Failed as expected:
```
src/Disks/tests/gtest_cas_ref_read_contract.cpp:200: Failure
Expected: (backend->headCount(layout.refCatalogKey())) > (0u), actual: 0 vs 0
[  FAILED  ] CasRefReadContract.HotRefReadsThroughHeldRuntimeIssueZeroCatalogRequests (0 ms)
```
Full output preserved at `build/t1a_sensitivity_test2.log`. Mutation reverted; the real assertion
(`EXPECT_EQ(..., 0u)`) restored and re-verified green in the final run.

### 3. `StaleLifeDropRefusesAfterRebirthAndNeverTouchesSuccessor`

Pins the one held ref-writer seam the classification found: `dropNamespace(const NamespaceLifeId &)`
→ `dropNamespaceImpl`'s `expected_incarnation` guard (`CasRefLedger.cpp` around line 4773). A ref is
published under life 1; life 1 is dropped and life 2 admitted at the same name (same sequence as
test 1); a different ref is published under life 2. The catalog object's HEAD token and full GET
bytes are captured before the call. `store->dropNamespace(life1)` (the exact-life overload) is then
driven and asserted to throw with `ErrorCodes::NETWORK_ERROR` (`expectThrowsCode`) — this is the
retry-later class the production code actually throws (`throwCasWriteRetryLater`), not a
`LOGICAL_ERROR`, so no death-test split is needed. After the refused call, the catalog object's
token and bytes are asserted byte-for-byte and token-for-token identical to before (the guard fires
before any write, so nothing should have changed at all), and a fresh mount over the same backend
(`verify_store`, a distinct `server_root_id`, mirroring
`RefWriterRuntimeIdentity.ColdReadRejectsReplacementByExternalPoolActor`'s `external_store` pattern)
still resolves the ref to life 2's exact `ManifestId`.

Result: **PASS on the first run** (coverage pin).

Sensitivity check: temporarily replaced `expectThrowsCode(NETWORK_ERROR, ...)` with
`EXPECT_NO_THROW(store->dropNamespace(life1))` (expect the stale-life drop to succeed). Failed as
expected:
```
src/Disks/tests/gtest_cas_ref_read_contract.cpp:239: Failure
Expected: store->dropNamespace(life1) doesn't throw an exception.
  Actual: it throws DB::Exception with description "CAS write could not be committed (CAS namespace
  '00/ref_read_contract_stale_drop@cas@': exact removal life ... differs from current catalog life
  ...); retrying later".
[  FAILED  ] CasRefReadContract.StaleLifeDropRefusesAfterRebirthAndNeverTouchesSuccessor (0 ms)
```
Full output preserved at `build/t1a_sensitivity_test3.log`. Mutation reverted; the real assertion
(`expectThrowsCode`) restored and re-verified green in the final run.

## Build and run log

- Initial build + red/coverage run: `build/t1a_red_build.log`, `build/t1a_first_run.log` — 2/3
  passed on the first attempt; test 2's positive control (a `listCount` assertion on the
  namespace stream prefix) genuinely failed, because `ensureRefTableRecovered`'s cold arm reads
  the namespace checkpoint via `readCkpt` (`backend.get(layout.refCkptKey(life))`), not a LIST of
  the stream prefix. Corrected in the same commit's diff (not a separate fix round — this is the
  test's own instrumentation, found before any production claim was made).
- Corrected build + green run: `build/t1a_green_build.log`, `build/t1a_green_run.log` — 3/3 PASS.
- Per-test sensitivity builds/runs: `build/t1a_sens1_build.log` /
  `build/t1a_sensitivity_test1.log`, `build/t1a_sens2_build.log` / `build/t1a_sensitivity_test2.log`,
  `build/t1a_sens3_build.log` / `build/t1a_sensitivity_test3.log` — each shows the wrong-expectation
  variant failing for the stated reason.
- Final build + run after all mutations reverted: `build/t1a_final_build.log`,
  `build/t1a_final_run.log` — 3/3 PASS.
- Neighbor suites (`CasNamespaceFileReadContract.*:CasRefCatalogRemoval.*`):
  `build/t1a_neighbor_run.log` — 8/8 PASS, no regression.

## Backlog observations from the classification (not acted on here)

1. **The classification's claim about `readable_catalog_after_observation_hook_for_test` is
   FALSE — it is NOT unused.** The classification report states (§"Site 2 discussion") that this
   hook "currently has zero test users (tree-wide grep)". A grep of the actual tree shows it is
   used by at least three tests in `src/Disks/tests/gtest_cas_ref_writer.cpp`
   (`RefWriterRuntimeIdentity.ColdReadRejectsReplacementByExternalPoolActor` and two neighboring
   tests in the same suite), each of which drives exactly the race the classification says the
   hook exists for — a catalog mutation injected between the cold arm's two `CasRefCatalog::read`
   calls in `acquireReadableRefTableRuntime`. The classification's grep must have missed these
   (wrong search term, wrong directory scope, or run before that file's test additions). This is a
   PROSE finding in the classification document itself; it does not change T1a's disposition (site
   2 is still KEEP either way, as the classification itself notes), but the classification's
   "zero test users" sentence should be corrected or removed rather than repeated in a later
   document.
2. **`dropNamespaceImpl`'s exact-incarnation refusal, cited by the classification as untested, is
   now pinned** by this slice's test 3 (`StaleLifeDropRefusesAfterRebirthAndNeverTouchesSuccessor`).
   No further action needed on that observation.

## Comment policy and style

Comments state invariants and reasons only — no task numbers, plan/audit/review references.
Allman braces throughout. No `sleep`. No `EXPECT_THROW` on a `LOGICAL_ERROR` site — the one thrown
exception in this file's assertions (`dropNamespaceImpl`'s retry-later refusal) is
`ErrorCodes::NETWORK_ERROR`, asserted via `expectThrowsCode`, so no death-test split applies.
