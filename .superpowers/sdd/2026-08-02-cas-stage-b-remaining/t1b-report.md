# T1b slice report — namespace-file `list`-arm pin and Task 9 evidence re-check

Slice T1b, plan task T1 (`docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`, `{#t1b}`).
Consumes T1a's landed conventions (`ed76b256a50`, `ec0cfbc1007`;
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t1a-report.md`). **Zero production edits** —
the new test is a coverage pin, matching the audit's prediction that
`CasPlainObjects::listNamespaceFiles` derives its LIST prefix from
`layout.namespaceFilesPrefix(life)` and never touches `layout.refCatalogKey()`.

## The new test

`src/Disks/tests/gtest_cas_ns_file_read_contract.cpp`, suite `CasNamespaceFileReadContract`:

### `ListThroughHeldLifeIssuesZeroCatalogRequests`

Opens a `Pool` over a `CountingBackend` (the same idiom `CasNamespaceFileRequestProfile` uses),
resolves one `NamespaceLifeId` via `NamespaceLifeId::stageATransition`, writes two namespace files
under it (`putNamespaceFile`), resets the counting journal, then calls
`store->listNamespaceFiles(life)` — the `Pool` surface named in the brief. Asserts:

- both file names come back (order-independent, sorted before comparison);
- a positive control: `backend->listCount(prefix)` — where `prefix` is
  `layout.namespaceFilesPrefix(life)` — is nonzero, so the journal is proven alive for this call
  before the zero claims below are read as meaningful;
- zero requests of every write/read shape against `layout.refCatalogKey()`
  (`headCount`, `getCount`, `casPutCount`, `putCount`, `putOverwriteCount`).

**Result: PASS on the first run** — a coverage pin, not a fix, exactly as the audit predicted.

**Sensitivity check** (mandatory wording): load-bearing mutation demonstration performed after
implementation; mutation reverted; patch and failing output preserved. The mutation temporarily
changed `EXPECT_EQ(backend->headCount(refCatalogKey()), 0u)` to
`EXPECT_GT(backend->headCount(refCatalogKey()), 0u)` (expect a nonzero catalog HEAD on the list
path). It failed as expected:

```
src/Disks/tests/gtest_cas_ns_file_read_contract.cpp:245: Failure
Expected: (backend->headCount(store->layout().refCatalogKey())) > (0u), actual: 0 vs 0
[  FAILED  ] CasNamespaceFileReadContract.ListThroughHeldLifeIssuesZeroCatalogRequests (0 ms)
```

Full output preserved at `build/t1b_sensitivity_list.log`. Mutation reverted; the real assertion
(`EXPECT_EQ(..., 0u)`) restored and re-verified green in the final run
(`build/t1b_final_run.log`).

No genuine RED/fix step was needed — the test never failed against the unmutated production code.

## Task 9 claim re-check

The closure note `docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md`
(`{#read-and-delayed-write-aliasing}`) cites
`CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey` as proof
that "steady namespace-file operations add no hot-path catalog request." That cited test drives
disk-level rewrite/append/read/rotation operations (rotation itself enumerates existing dedup-log
segments, which reaches `listNamespaceFiles` transitively through
`ContentAddressedTransaction`) and asserts, via a `RecordingObjectStorage`, that none of those
operations' underlying requests match a catalog/ref/blob/manifest key family. The `list` arm was
therefore already exercised and covered by that assertion before this slice — just indirectly,
through disk-level rotation, and without a dedicated positive control on the LIST call itself.

**Task 9 claim re-checked against the new list-arm pin; fact unchanged; no evidence correction
needed.** The new `CasNamespaceFileReadContract.ListThroughHeldLifeIssuesZeroCatalogRequests` adds
a direct, `Pool`-level pin with its own positive control, but it confirms rather than contradicts
the cited test's claim. Nothing was appended to the closure note.

## Name-equivalence mapping

The plan's originally-specified test names, mapped to the tests actually committed in this file
(T1a landed the first two; this slice adds the third):

| Originally specified | Committed as |
|---|---|
| `StaleReaderAfterRebirthNeverSeesNewIncarnation` | `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes` |
| `DelayedWriterFinalizedAfterRebirthWritesOnlyItsOwnIncarnation` | `DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes` |
| `NamespaceFileHotPathsIssueZeroCatalogRequests` (list arm) | `ListThroughHeldLifeIssuesZeroCatalogRequests` |

## Build and run log

- Build + first (coverage-pin) run: `build/t1b_build.log`, `build/t1b_run.log` — 3/3 PASS,
  including the new test on its first attempt.
- Neighbor suites (`CasNamespaceFileDiskProfile.*:CasNamespaceFileRequestProfile.*`):
  `build/t1b_neighbor_run.log` — 8/8 PASS, no regression.
- Sensitivity mutation build + failing run: `build/t1b_sens_build.log`,
  `build/t1b_sensitivity_list.log` — 0/1 PASS (expected failure, shown above).
- Final build + run after the mutation was reverted: `build/t1b_final_build.log`,
  `build/t1b_final_run.log` — 3/3 PASS.

## Comment policy and style

Comments state invariants and reasons only — no task numbers, plan/audit/review references.
Allman braces throughout. No `sleep`. The test throws nothing itself, so no `EXPECT_THROW` /
death-test question arises.

## Commit

One commit: the test file plus this report (`git add -f`, per convention for the
`.superpowers/sdd/` tree). Subject: `ca: ref — namespace-file list arm pinned catalog-free`.
