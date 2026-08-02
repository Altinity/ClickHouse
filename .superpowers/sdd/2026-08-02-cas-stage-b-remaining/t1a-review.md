# T1a slice review — commit `ed76b256a50`

Reviewer verdict: **APPROVE-WITH-NONBLOCKING**.

Scope: the commit's three files (`src/Disks/tests/gtest_cas_ref_read_contract.cpp`,
`t1a-classification.md`, `t1a-report.md`), checked against plan task T1 (`{#t1}`, `{#t1a}`, the
read-side contract block and Global Constraints) and against the production code itself, not the
reports' description of it. All findings are PROSE except one nonblocking record gap I closed by
running the missing suites myself; **no CODE/TEST defect was found**, so no fix round is opened.

## 1. Spec compliance

All five obligations are met.

**(a) Rebirth-alias pin — delivered and non-vacuous.**
`HeldRuntimeAfterSameNameRebirthReadsStaleOrNotFoundNeverSuccessorRefs` publishes through the real
write path (`beginPartWrite`/`stageManifest`/`precommitAdd`/`promote`), holds the runtime with one
`resolveRef`, then drops and re-admits life 2 *outside* the store (`casUpdate` to `Removing`,
`deleteCompletedRemoving` under a held fence, `casAdmitEntry`), and publishes a different value
under life 2 via `publishCommittedTransition`.

Two things make the "does it really go through the warm path" question answerable without
guessing. First, `appendRefLogSeed` resolves the life on record (`resolveLifeOrSentinel`), so life
2's transition genuinely lands under life 2's physical prefix, not life 1's. Second, if the runtime
had *not* been warm, `acquireReadableRefTableRuntime`'s cold arm would have resolved the current
catalog row — life 2 — and the `EXPECT_NE` would fail rather than pass. The stale arm is also
demonstrably taken, not merely permitted: the preserved sensitivity log
(`build/t1a_sensitivity_test1.log`) prints a *present* 48-byte `held_after` value distinct from
`life2_manifest`, i.e. life 1's committed value was actually returned. The eviction positive
control (`ASSERT_FALSE(refTableCachedForTest(ns))` then a re-resolve to life 2) is a real
production re-recovery, not a second mount.

**(b) Zero-catalog-GET pin — delivered; the reset is correctly ordered.**
`HotRefReadsThroughHeldRuntimeIssueZeroCatalogRequests` establishes warmth *before*
`resetCounts()`: publish, then one `resolveRef`, then the positive controls (catalog head/get/casPut
sum > 0 and a `refCkptKey(life)` GET > 0) are captured pre-reset, then the reset, then
`resolveRef`/`listRefs`/`hasAnyRefWithPrefix`. This ordering is load-bearing for a reason the test
does not spell out but which holds: the pre-reset read is also what consumes
`needs_stale_precommit_sweep`, so the post-reset reads cannot piggyback the sweep's writes.
`CountingBackend::resetCounts` clears every counter map, so `touchedKeys()` being empty afterwards
is a real absence.

Not flaky: the background snapshot publisher is admitted only over
`snapshot_log_count_threshold = 256` / `snapshot_log_bytes_threshold = 1 MiB`
(`admitSnapshotPublishUnderStateLock`), and the test writes ~3 transactions — the trigger is
count/byte based, with no wall-clock term, so no off-thread write can race the assertion.

**(c) Stale-writer pin at the one held-writer seam — delivered, with the error code verified at the
throw site.** `store->dropNamespace(life1)` reaches `CasRefLedger::dropNamespaceImpl`, whose
`expected_incarnation` guard calls `throwCasWriteRetryLater`, which throws
`ErrorCodes::NETWORK_ERROR` (`Backend/CasRequestControl.cpp`, `throwCasWriteRetryLater`). The
implementor's claim is correct; `expectThrowsCode(NETWORK_ERROR, …)` is the right assertion and no
death-test split applies.

**(d) Zero production edits — confirmed.** `git show --stat` lists exactly three files: the new test
and the two slice documents.

**(e) Honest recording — confirmed.** Both coverage pins are recorded as PASS-on-first-run, not as
manufactured REDs; the third is likewise a pin. All three sensitivity logs were read and show the
wrong-expectation variant failing for the stated reason (see §3). The report also discloses that
test 2's *first* positive control (a `listCount` on the stream prefix) genuinely failed because
cold recovery reads `refCkptKey` via `readCkpt` rather than LISTing — that disclosure is exactly
the kind of instrumentation error that usually goes unmentioned.

**Classification checked against the code.** The load-bearing claim — the warm path of
`acquireReadableRefTableRuntime` returns before any catalog read — is true: the
`lookupRefTableRuntime` hit path performs the fence check, the `removal_admission_closed` check and
the invalidation/supersession checks, then returns; the first `CasRefCatalog::read` is below that
early return.

Spot-checks of the sites the dispatch named, all defensible:

- Site 2 (`acquireReadableRefTableRuntime` confirm read) — class 1, KEEP. It is not a class-5
  duplicate: the comparison `second_catalog.token != first_catalog.token || catalog !=` is
  load-bearing and aborts admission via `throwCasWriteRetryLater`.
- Sites 4/5 (`commitRefChunk`, removal and positive arms) — class 2, KEEP. The removal arm requires
  the exact life to still be `Removing` before id allocation; the positive arm requires it still
  `Live` and permanently closes the local positive lane otherwise. Both are mutation authority.
- Site 8 (`dropNamespaceImpl` initial read) — class 1, KEEP, and it is precisely the seam the
  `expected_incarnation` guard fires at.
- Site 9 (pre-`beginRemoving`) — class 2, KEEP, and genuinely not a duplicate of site 8: it is
  taken *after* an unbounded `cv.wait` drain and its incarnation equality is checked, not assumed.
- Site 10 (catch block) — class 2, KEEP; fail-close, reopening the lane only on a fresh observation
  proving the identical original `Live` row.

Two further checks the classification depends on and that hold: `resolveNamespaceLife`'s bound is
indeed `kMaxResolveAttempts = 32`; and the §(a) enumeration of read entry points is complete —
`confirmExactRef`, the one other read-shaped API, is documented and implemented to do zero object
store I/O and never even materializes a runtime, so it reaches none of the ten sites.

## 2. Code/test quality

**Sanitizer hazard sweep — clean.**
`grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR" src/Disks/tests/gtest_cas_ref_read_contract.cpp`
returns nothing. The file's `LOGICAL_ERROR` uses are fixture-internal *throws* (bad fixture state),
never expectations.

**Would each test fail if its property regressed?** Yes, and the mechanism is traced above for each:
test 1 fails on a cold re-resolution (which is what a lost warm path looks like); test 2 fails on any
counted catalog request; test 3 fails if the incarnation guard is removed (the sensitivity variant
shows exactly that). Test 3 also checks the catalog object's HEAD token *and* full GET bytes
before/after, and re-resolves life 2's exact `ManifestId` from an independent mount
(`verify_store`, distinct `server_root_id`) — so both halves the dispatch asked about are real.

**Comment policy — one violation, see F3.** Everything else in the file states reasons and
invariants; Allman braces throughout; no `sleep`.

## 3. Sensitivity logs

All three read and genuine, each failing at the mutated assertion and only there:

- `build/t1a_sensitivity_test1.log` — `EXPECT_EQ(held_after, life2_manifest)` fails with two
  distinct 48-byte values (this is also the evidence that the stale read returned life 1's value).
- `build/t1a_sensitivity_test2.log` — `EXPECT_GT(headCount(refCatalogKey()), 0u)` fails `0 vs 0`.
- `build/t1a_sensitivity_test3.log` — `EXPECT_NO_THROW(dropNamespace(life1))` fails with the exact
  "exact removal life … differs from current catalog life …" retry-later exception.

## 4. Independent re-verification at HEAD

Rebuilt and re-ran rather than relaying the implementor's logs:

- `build/t1a_review_build.log` — `NINJA_EXIT=0`.
- `build/t1a_review_test.log` —
  `CasRefReadContract.*:RefWriterRuntimeIdentity.*:CasNamespaceFileReadContract.*:RefTableCacheEviction*`
  → 15/15 PASS.
- `build/t1a_review_gc_authority.log` — `CasRefGcCleanupAuthority.*` → 4/4 PASS.

## Findings

### F1 — PROSE, FALSE. The implementor is right about the hook.

`t1a-classification.md` §"Site 2 discussion" and §(d) state that
`readable_catalog_after_observation_hook_for_test` "currently has **zero test users** (tree-wide
grep)". It has three, all in `src/Disks/tests/gtest_cas_ref_writer.cpp`:
`RefWriterRuntimeIdentity.ColdReadRejectsCatalogLifeReplacedWithoutLocalInvalidation`,
`…ColdReadRejectsReplacementByExternalPoolActor`, `…ColdReadRejectsUnrelatedCatalogMutationBetweenObservations`
— each driving exactly the between-the-two-reads race the classification says the hook exists for.
Consequently the §(c) remark that removing site 2 "would be noticed by nothing today" is also false:
those three tests would notice. The disposition is unaffected (site 2 is KEEP either way).

### F2 — PROSE, IMPRECISE. "A live table reader issues zero catalog GETs" is unqualified.

`t1a-classification.md` §(a) says "a live table reader with a held runtime issues zero catalog GETs
today". True of the steady-state warm read that test 2 pins, but not of every read: `resolveRef`,
`listRefs` and `hasAnyRefWithPrefix` all call `sweepStalePrecommitsForRead`, and when
`needs_stale_precommit_sweep` is armed that sweep appends through `appendRefOps` →
`commitRefChunk`, i.e. reaches site 5. The contract's own read/mutation split makes this harmless
(a piggybacked sweep is a mutation, and its read is class 2), but the sentence claims more than the
code supports and would mislead a later reader into treating any read-path catalog GET as a
regression.

The same imprecision appears in the test file's comment "a warm ref read is a pure map lookup …, so
it issues no backend request whatsoever": that holds once the maintenance flags are disarmed, which
is a property of the test's setup, not of every warm read. Secondary detail: `touchedKeys()` covers
head/get/getStream/put/putOverwrite/casPut/list/delete but not `putIfAbsentStream` (not overridden
by `CountingBackend`), so "no backend request whatsoever" is checked over the counted ops only.

### F3 — PROSE (comment), Global-Constraints violation. Task id in a source comment.

`src/Disks/tests/gtest_cas_ref_read_contract.cpp` opens with `/// T1a of the ref-side read contract
(…)`. Global Constraints: "No comment may reference plans, reviews, task numbers, or BACKLOG
anchors." `T1a` is a plan slice id and will dangle once the plan is deleted from the branch. The
reason in that comment is durable and worth keeping; only the identifier should go — e.g. "The
ref-side read contract (the ten `CasRefCatalog::read` sites …)".

This also makes `t1a-report.md`'s §"Comment policy and style" claim — "no task numbers, plan/audit/
review references" — FALSE as written.

I label this PROSE because the plan defines PROSE as "comments, docs, plan text" and routes it to
the batch; flagging so the controller can decide, since it is a one-word deletion in a file that is
otherwise clean.

### F4 — PROSE, process omissions in the record.

Two Step-6/Global-Constraints details the record misses. (i) The mandatory mutation-demonstration
wording ("load-bearing mutation demonstration performed after implementation; mutation reverted;
patch and failing output preserved.") does not appear in `t1a-report.md`; the substance is fully
there, only the mandated sentence is absent. (ii) The plan's Step 6 asks the commit message to name
the report; `ed76b256a50` has a subject line and no body. The subject itself is an honest and better
description than the plan's suggested one (no production edits happened, so "held-life ref reads"
would have overclaimed) — that deviation is an improvement, not a defect.

### F5 — TEST record gap, closed by this review.

Plan Step 5 specifies the no-regression run as
`CasRefReadContract.*:CasRefGcCleanupAuthority.*:RefTableCacheEviction*`, with the
`CasRefGcCleanupAuthority` green explicitly called out as the proof that class-3 revalidation reads
survived. `t1a-report.md` records a *different* neighbor set
(`CasNamespaceFileReadContract.*:CasRefCatalogRemoval.*`, 8/8). With zero production edits the
regression risk was nil, but the specified evidence was missing from the record. I ran both suites
at HEAD: `CasRefGcCleanupAuthority.*` 4/4 PASS (`build/t1a_review_gc_authority.log`) and
`RefTableCacheEviction*` PASS within `build/t1a_review_test.log`. Gap closed; nothing to fix.

### F6 — TEST, nonblocking suggestion (not a defect).

Test 3 runs on `InMemoryBackend`, so "never touches the successor" is pinned by catalog
token+bytes equality plus life 2 still resolving to its exact `ManifestId`. That is sound for what
the test claims. A `CountingBackend` would additionally let it assert `deleteTotal() == 0` and zero
writes under life 2's prefix, which is strictly stronger for the same effort. Worth doing only if
this file is touched again.

### Observation — pre-existing overlapping coverage, unmentioned.

`RefWriterRuntimeIdentity.WarmReadableRuntimeDoesNotReadCatalog` already pinned a weaker form of
test 2 (empty fixture-admitted table, `getCount(refCatalogKey()) == 0` only). The new test is
strictly stronger — a real published ref, all five op counters, all three read entry points, the
`touchedKeys()` claim, and explicit positive controls — so it is not redundant. Neither slice
document mentions the pre-existing test; a later gate looking for duplicate coverage would find it
and wonder. Worth one sentence in the report.

## Verdict

**APPROVE-WITH-NONBLOCKING.** The slice delivers (a)–(e); the all-KEEP classification is correct
where I checked it against the code, including the four sites the dispatch singled out; the three
tests are sensitive, correctly ordered, and free of the sanitizer hazard; the implementor's FALSE-claim
flag (F1) is upheld. Nothing here opens a fix round: F1, F2, F3 and F4 are PROSE for the
`docs/superpowers/cas/deferred-docs-fixes.md` batch, F5 is closed by the runs recorded above, and F6
is an optional strengthening.
