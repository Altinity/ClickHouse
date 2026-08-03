# T1b slice review — `ListThroughHeldLifeIssuesZeroCatalogRequests`

Reviewed commit: `be3394a1528` ("ca: ref — namespace-file list arm pinned catalog-free").
Normative input: plan slice `{#t1b}` + Global Constraints in
`docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`.
Slice artifact reviewed: `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t1b-report.md`.

## Verdict

**APPROVE-WITH-NONBLOCKING.**

Zero production edits (confirmed against the diff: only
`src/Disks/tests/gtest_cas_ns_file_read_contract.cpp` and the report). The new test is
spec-compliant on its essential claim and is genuinely sensitive to the regression it names — but I
had to establish that sensitivity myself, because the report's own sensitivity evidence is
tautological. Four findings below: two TEST (both minor, neither blocking), two PROSE.

## Independent run

```
flock "$(git rev-parse --git-common-dir)/unit_tests.lock" ./build/src/unit_tests_dbms \
  --gtest_filter='CasNamespaceFileReadContract.*:CasNamespaceFileDiskProfile.*:CasNamespaceFileRequestProfile.*'
```
→ **11/11 PASS** (3 `CasNamespaceFileReadContract` + 3 `CasNamespaceFileDiskProfile` +
5 `CasNamespaceFileRequestProfile`). Log: `build/t1b_review_run.log`. The binary
(`build/src/unit_tests_dbms`, mtime 00:59) is the post-revert build, HEAD is the commit under
review, and the test file is unmodified in the worktree — so the run is about this tree. The
report's "neighbors 8/8" is confirmed (3 + 5).

## What I verified as correct

**The held life is real.** `life` is resolved once, outside the call under test, and
`Pool::listNamespaceFiles` → `CasPlainObjects::listNamespaceFiles` takes `const NamespaceLifeId &`
and re-resolves nothing. Its only backend contact is
`backend.list(layout.namespaceFilesPrefix(life), cursor, 1000)` in the paging loop. No catalog
path exists on that call.

**The pin is sensitive** (established statically, since the report's demonstration is not — see
finding 1). Every production access to the catalog object is against `layout.refCatalogKey()` via
`backend.get` (`CasRefCatalog::read`, and the `ensure` path's re-read), `backend.putIfAbsent` (the
canonical-empty seed) and `backend.casPut` (`casUpdateImpl`). All three shapes are asserted zero by
the test. There is no `getStream` read of the catalog anywhere in production, so the `getCount`
assertion is not blind to the realistic regression. If `listNamespaceFiles` grew a
`CasRefCatalog::read`, `getCount(refCatalogKey())` becomes nonzero and the test fails.

**Not vacuous.** Both file names are asserted returned, and the positive control
`EXPECT_GT(backend->listCount(prefix), 0u)` is live — I confirmed `CountingBackend::list` keys its
counter on the *prefix* argument, which is exactly `layout.namespaceFilesPrefix(life)`.

**`resetCounts` placement is right.** `Pool::open` seeds the catalog with a
`putIfAbsent(refCatalogKey(), ...)` and reads it; those land before the reset, so they neither mask
the assertions nor falsely trip them. This matches what the neighbor file's `openCountedPool`
helper does for the same reason.

**T1c seam swap survivable.** `life` is used only as an identity: prefix derivation,
`putNamespaceFile`, `listNamespaceFiles`. The test never admits it to the catalog, never calls
`resolveLifeOrSentinel`, and depends on nothing about `stageATransition` beyond the identity being
stable within the test body. A seam that constructs the same bytes locally (or any stable identity)
substitutes mechanically.

**Sanitizer sweep.** `grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"` on
`src/Disks/tests/gtest_cas_ns_file_read_contract.cpp` → no hits. No death-test question arises.

**Comment policy.** The new comment cites no task number, plan, review, or BACKLOG anchor. Allman
braces. No `sleep`.

## Findings

### 1. TEST (minor, non-blocking) — the sensitivity demonstration is tautological

The mutation recorded in the report and preserved in `build/t1b_sensitivity_list.log` changes
`EXPECT_EQ(backend->headCount(refCatalogKey()), 0u)` to
`EXPECT_GT(backend->headCount(refCatalogKey()), 0u)` — a flip of the assertion under test, not a
mutation of the behaviour it guards. That flip *must* fail whenever the original passes; it proves
only that the counter reads 0, which the passing test already said. It establishes nothing about
whether the assertion would catch a production regression.

The load-bearing mutation is in the subject: insert a `backend.get(layout.refCatalogKey())` (or a
`CasRefCatalog::read`) into `CasPlainObjects::listNamespaceFiles` and show the pin goes red. I did
not perform it (read-only on source in a shared worktree) and established sensitivity by the static
argument above instead. The conclusion holds; the recorded evidence does not support it.

### 2. TEST (minor, non-blocking) — the zero-assertion set omits three counted shapes

The brief enumerates "zero requests of EVERY counted shape … head/get/casPut/list/delete". The test
asserts `headCount`, `getCount`, `casPutCount`, `putCount`, `putOverwriteCount` and omits
`listCount(refCatalogKey())`, `deleteCount(refCatalogKey())` and `getStreamCount(refCatalogKey())`
— all three counters exist on `CountingBackend`. This is not a live hole (no production path lists,
deletes, or streams the catalog key), so it does not weaken the pin today; it is a completeness gap
against the brief and against a future op family.

The neighbour already shows the shape-agnostic form. `CasNamespaceFileRequestProfile.DedupLogRotation`
asserts `EXPECT_EQ(backend->touchedKeys(), (std::vector<String>{prefix, old_key, new_key}))`. Here
the equivalent would be `EXPECT_EQ(backend->touchedKeys(), std::vector<String>{prefix})` — exact,
strictly stronger than the five zeros combined, and immune to a newly added operation family.

### 3. PROSE-IMPRECISE — the new test's comment claims more than its fence checks

The comment says listing "must cost **exactly that one LIST** and nothing against
`layout.refCatalogKey()`". The fence is `EXPECT_GT(backend->listCount(prefix), 0u)` with no
`listTotal()` bound: it passes on ten LISTs of that prefix, and passes on one LIST of that prefix
plus any number against other prefixes. The neighbour pins the same sentence properly with
`EXPECT_EQ(backend->listCount(prefix), 1u)` **and** `EXPECT_EQ(backend->listTotal(), 1u)`. Either
tighten the two assertions or soften the comment to what a positive control actually claims.

### 4. PROSE-FALSE — the Task 9 re-check reaches a right verdict on a wrong ground

The report states that the cited test "drives disk-level rewrite/append/read/rotation operations
(rotation itself enumerates existing dedup-log segments, which reaches `listNamespaceFiles`
transitively through `ContentAddressedTransaction`)" and concludes "the `list` arm was therefore
already exercised and covered by that assertion before this slice". Traced against the code, this
is false.

In `CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey` the
"rotation" is a write of `deduplication_log_2.txt` followed by
`ContentAddressedTransaction::unlinkFile` of `deduplication_log_1.txt`. The verbatim-table-file arm
of `unlinkFile` calls `readableNamespaceFilesLife`, `getNamespaceFile` and `removeNamespaceFile`;
it never enumerates. The complete set of production callers of `listNamespaceFiles` is: the
table-subdirectory arm of `removeRecursive`, the RENAME TABLE arm of the move path,
`existsDirectory` for `DirShape::TableSubdir`, and `listDirectory` for `DirShape::TableDir` and
`DirShape::TableSubdir`. That test drives none of them. Its sibling
`RemovalOnANeverOpenedTableLeavesTheCatalogUntouched` does call `removeRecursive` on the
subdirectory, but on a never-opened table it returns at the absent-life guard, before the
enumeration.

So the list arm was **not** previously covered by that catalog-negative assertion — which raises
this slice's value rather than lowering it. The re-check verdict itself stands on other grounds:
the closure sentence claims steady namespace-file operations add no hot-path catalog request, that
remains true, and it is now better supported, so no note edit was owed.

A second, smaller error in the same paragraph: the claim that the list arm was covered "without a
dedicated positive control on the LIST call itself" is also false about the tree —
`CasNamespaceFileRequestProfile.DedupLogRotation` has carried exactly that
(`EXPECT_EQ(backend->listCount(prefix), 1u)` plus a `touchedKeys()` set assertion) at the same
`Pool` layer.

### 5. Note for the deferred-docs batch (PROSE-IMPRECISE, pre-existing)

`docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md`
(`{#read-and-delayed-write-aliasing}`) says
`CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey` "proves
that steady namespace-file operations add no hot-path catalog request." Read as universal over
namespace-file operations, that overstates the cited test, which drives rewrite, append, read and
unlink only (per finding 4). After this slice the *fact* is fully supported — but by two tests, not
one. The precise repair is to add the new test to the citation, not to change the claim.

## Report completeness (T1a conventions)

Present and correct: the test description, the verbatim mandatory mutation-demonstration wording,
the Task 9 re-check section, the name-equivalence mapping (three rows, matching the tests actually
in the file), the per-artifact build/run log list, and the comment-policy/style section. The plan's
T1b step 4 does not require a full CA-gate count (that is T1's step 4, after T1c), so its absence is
correct. The one substantive defect is finding 4, inside the Task 9 section.
