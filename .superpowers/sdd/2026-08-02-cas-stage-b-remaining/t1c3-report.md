# T1c commit 3/3 — sentinel retirement, and the T1b review fixes

Two commits, in order.

## Commit A — T1b review fixes

`fd7af51a992` ("ca: tests — namespace-file list pin strengthened to the exact touched-set
form"), modifying only `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp`.

`ListThroughHeldLifeIssuesZeroCatalogRequests` previously asserted five separate zero-counters
against `layout.refCatalogKey()` (`headCount`, `getCount`, `casPutCount`, `putCount`,
`putOverwriteCount`). Per the T1b review (finding 2), that enumeration is blind to any newly added
request shape against the catalog key it does not name. It is now the shape-agnostic form the
neighbor `CasNamespaceFileRequestProfile.DedupLogRotation` already uses:

```cpp
EXPECT_EQ(backend->listCount(prefix), 1u);
EXPECT_EQ(backend->listTotal(), 1u);
EXPECT_EQ(backend->touchedKeys(), std::vector<String>{prefix});
```

The comment above the test (finding 3) previously claimed the fence proved "exactly that one LIST
and nothing against `layout.refCatalogKey()`" while the fence itself was only `EXPECT_GT(...,
0u)`. It now reads "must cost exactly one LIST of the files prefix and nothing else", matching
what the strengthened assertions actually check.

**Load-bearing mutation demonstration performed after implementation** (finding 1: the review
found the previously recorded demonstration tautological — it flipped the assertion under test
rather than mutating the behaviour it guards). A temporary `backend.get(layout.refCatalogKey());`
was inserted at the top of `CasPlainObjects::listNamespaceFiles` in
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPlainObjects.cpp`, the
binary was rebuilt, and the test went RED for the right reason:

```
src/Disks/tests/gtest_cas_ns_file_read_contract.cpp:246: Failure
Expected equality of these values:
  backend->touchedKeys()
    Which is: { "p/cas/ns/state/138c597bef827b0cd3d211467e556cd7/_files/", "p/cas/ref_catalog" }
  std::vector<String>{prefix}
    Which is: { "p/cas/ns/state/138c597bef827b0cd3d211467e556cd7/_files/" }
```

(preserved in `build/t1c3_mutation_list.log`). The mutation was then reverted; `git status`/`git
diff` confirmed `CasPlainObjects.cpp` was back to its committed state before the rebuild; the
suite (`CasNamespaceFileReadContract.*:CasNamespaceFileDiskProfile.*:CasNamespaceFileRequestProfile.*`,
11 tests) was rebuilt and re-verified green in `build/t1c3_regreen_test.log`.

Logs: `build/t1c3_commitA_build.log`, `build/t1c3_commitA_test.log` (pre-mutation green, 11/11),
`build/t1c3_mutation_build.log`, `build/t1c3_mutation_list.log` (the RED run), `build/t1c3_regreen_build.log`,
`build/t1c3_regreen_test.log` (post-revert green, 11/11).

## Commit B — sentinel retirement

### 1. Fixture seam made self-contained

`src/Disks/tests/cas_test_helpers.h`: `fixture::fixtureLife` no longer delegates to
`NamespaceLifeId::stageATransition`. It now reproduces the identical derivation locally: a
`sipHash128` of the namespace's string bytes, the same zero-guard (map a zero hash to `1`), and
`NamespaceLifeId::fromCatalogEntry(ns, incarnation)` to pair it with the namespace. This is
byte-for-byte the same value `stageATransition` produced, so every existing fixture key is
unchanged. A forward declaration of `fixture::fixtureLife` was added next to the file's existing
forward-declaration block (`appendRefLogSeed`, defined earlier in the file, calls it before the
`fixture` namespace's first definition later in the file — the same reason the file already
forward-declares `writeRefLogTxnRaw` there).

The three helper-internal calls to `CasRefCatalog::resolveLifeOrSentinel` (in `appendRefLogSeed`,
`writeRefSnapshotRaw`, and `writeRefLogTxnRaw`) are now
`CasRefCatalog::lifeIfCataloged(backend, layout, ns).value_or(fixture::fixtureLife(ns))` —
semantics preserved exactly: an already-admitted catalog entry wins, otherwise the fixture
identity. The adjacent comments were updated to stop naming the deleted function; one comment
block in `casAdmitEntry`'s docstring that discussed `stageATransition` and a stale "BRIDGE, not a
resting place ... Task 6" paragraph (describing this retirement as future work) was deleted rather
than patched, since patching it in place would have left prose about a bridge that no longer
exists.

### 2. Production deletions

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h` /
  `.cpp`: deleted `resolveLifeOrSentinel` (declaration, definition, and its doc comment).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasNamespaceLifeId.h`:
  deleted `stageATransition` (declaration/definition and its two comments, including the "Task 6
  DELETES it" sentence). `<Common/SipHash.h>` was also removed from this file's includes — it was
  only used by the deleted factory.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h` and `.cpp`: the two
  comments naming `resolveLifeOrSentinel` were rewritten to state the invariant ("use the SAME
  complete catalog cut ... never an independent catalog re-read") without naming the now-deleted
  function.

### 3. Zero-grep gates

```
$ git grep -n "stageATransition" -- 'src/' ':!*.md'
(no output, exit 1)

$ git grep -n "resolveLifeOrSentinel" -- 'src/' ':!*.md'
(no output, exit 1)
```

A tree-wide grep with no path restriction (`git grep -n "stageATransition\|resolveLifeOrSentinel"
-- . ':!*.md'`) also returned empty.

### 4. Lane-closure gates

**(a) Release — full CA gate**, under the shared lock, using the generated 277-suite filter:

```
wrote 277 suites to build/cas_suites.txt (21 excluded, 0 unclaimed)
[==========] 1976 tests from 277 test suites ran. (172725 ms total)
[  PASSED  ] 1975 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] WinnerShape/CasGcCompletedRemovalFenceRace.FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement, where GetParam() = 1-byte object <01>
 1 FAILED TEST
 YOU HAVE 2 DISABLED TESTS
```

Identical to the T1c2 baseline (`build/t1c2_gate.log`): same 1976 ran / 1975 passed / the one
known pre-existing `WinnerShape/CasGcCompletedRemovalFenceRace...Replacement` failure / 2
disabled. No other delta. Log: `build/t1c3_gate.log` (build log: `build/t1c3_build.log`, suite
generation: `build/t1c3_suites.log`).

**(b) ASan — full CA gate**, launched via `nohup` with the `T1C3_ASAN_DONE=` marker under the same
shared lock (queued behind the release run). The ASan build's `generate_cas_suites.sh` claimed 295
suites (debug build includes the death-test suites; 3 excluded, 0 unclaimed) — the filter is wider
than release's 277 by design, as expected.

The run reached test 1242 of the ASan suite list (1241 `[ OK ]`, one `[ RUN ]` with no matching
`[ OK ]`) and then the process aborted with exit code 134 (`SIGABRT`):

```
Logical error: 'CAS ref catalog: namespace 'live' is live and carries removal_started_round -- the
field is required iff state == Removing'.
...
5. .../Formats/CasRefCatalogFormat.cpp:133:19: DB::Cas::encodeRefCatalog(...)
6. src/Disks/tests/gtest_cas_ref_catalog.cpp:242:21: CasRefCatalogFormat_RemovalStartedRoundIsRequiredExactlyForRemoving_Test::TestBody()
...
T1C3_ASAN_DONE=134
```

This is `CasRefCatalogFormat.RemovalStartedRoundIsRequiredExactlyForRemoving` in
`src/Disks/tests/gtest_cas_ref_catalog.cpp:242`, an unguarded `EXPECT_THROW`/direct call against a
`LOGICAL_ERROR`-throwing site that aborts the process under `DEBUG_OR_SANITIZER_BUILD` instead of
throwing — the exact recurring hazard this repository's `LOGICAL_ERROR`/death-test-split rule
exists to catch. This test was introduced by `bf396ffa50d` ("ca: add generation-7 pre-fold
lifecycle core"), a commit unrelated to this task's diff, and is confirmed pre-existing (not
touched or introduced by commit A or commit B here). I did not touch it, per instruction: a
separate hygiene slice will split it (guard the throwing arm under `#ifndef
DEBUG_OR_SANITIZER_BUILD`, add an `EXPECT_DEATH` `Cas*DeathTest` arm under `#else`) and re-run the
full ASan gate.

Everything that DID run under ASan before the abort (1241 tests, spanning the CA suite list up to
and including `CasRefCatalogFormat.RoundTripsAllThreeStates`) passed — no failures, no other
aborts. This includes every suite this task's own diff touches
(`CasNamespaceFileReadContract.*`, `CasNamespaceFileDiskProfile.*`,
`CasNamespaceFileRequestProfile.*`, and the ~39 test files migrated onto `fixture::fixtureLife` in
the prior T1c commits, which this commit's helper-signature change also recompiles against).
Logs: `build_asan/t1c3_build.log`, `build_asan/t1c3_suites.log`, `build_asan/t1c3_gate.log`.

**Not run in this dispatch**: the T1 task-closure integration lane
(`test_content_addressed_s3 test_content_addressed_shared_pool`) and the task-level spec/quality
review named in plan step T1/4(b)/(c) — the dispatch for this report named only the release and
ASan lane-closure gates.

## Comment policy / style

Allman braces throughout the new/edited code. No `sleep`. No comment in the diff cites a plan,
task number, review, or `BACKLOG` anchor — the deleted "Task 6 DELETES it" and "BRIDGE ... Task 6"
sentences were removed rather than rewritten, since removal is what the comment policy prefers
when a sentence's only content is provenance that no longer applies.
