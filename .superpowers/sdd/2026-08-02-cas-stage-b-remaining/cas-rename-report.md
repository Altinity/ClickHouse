# CAS suite `Cas`-prefix normalization — report

Branch `cas-gc-rebuild`, main worktree `/home/mfilimonov/workspace/ClickHouse/master`.
Baseline HEAD `d32eceea73a`. Commits: `e977ab639a5`, `3d959928e06`, `c7a9b2c17bc`.

## Enumeration

Derived from the built binary, not from grep: `--gtest_list_tests` on both binaries, intersected with
the generator's emitted suite list, then set-differenced against what the `Cas*` prefix selects. Script
`build/cas_rename/count.py`. The enumeration produced **23 spellings** not reachable by `Cas*` —
identical in the release and ASan binaries — plus **one suite invisible to the generator entirely**,
for 21 suite renames and 3 instantiation-prefix renames.

The brief predicted 21 suites and 2 instantiation prefixes. Two differences, both found by the
enumeration:

- **`WinnerShape/CasGcCompletedRemovalFenceRace`** — a third `INSTANTIATE_TEST_SUITE_P` prefix, in
  `src/Disks/tests/gtest_cas_gc_frontier_gate.cpp`, the file already in scope for the
  `CatalogLifecycleReconciler` rename. Only the instantiation prefix moved; the suite
  `CasGcCompletedRemovalFenceRace` and its `TestWithParam` fixture class are untouched, so nothing in
  the `Cas*Gc*` suite families owned by the concurrent lane-g work is affected.
- **`ContentAddressedSettings`** — present in the binary with 6 tests, in a CAS test file, but absent
  from the generator's list because its file `gtest_content_addressed_settings.cpp` matched neither
  `gtest_ca*.cpp` nor `gtest_cas*.cpp`. **These 6 tests had never been gated.** Verified:
  `grep -c ContentAddressedSettings build/cas_rename/baseline_suites_release.txt` → 0, while
  `--gtest_list_tests` line 3764 shows `ContentAddressedSettings.`.

## Rename table

All 8 target files are under `src/Disks/tests/` (the brief placed two in `src/Interpreters/tests/`;
they are not there). Every rename touched only the **first argument of a `TEST` macro** — see the
production-symbol note below.

| old | new | file |
| --- | --- | --- |
| `RefWriterAppendLane` | `CasRefWriterAppendLane` | `gtest_cas_ref_writer.cpp` |
| `RefWriterListRefs` | `CasRefWriterListRefs` | `gtest_cas_ref_writer.cpp` |
| `RefWriterNamespaceBirth` | `CasRefWriterNamespaceBirth` | `gtest_cas_ref_writer.cpp` |
| `RefWriterNamespaceRemoval` | `CasRefWriterNamespaceRemoval` | `gtest_cas_ref_writer.cpp` |
| `RefWriterNonMinting` | `CasRefWriterNonMinting` | `gtest_cas_ref_writer.cpp` |
| `RefWriterPublishFromLive` | `CasRefWriterPublishFromLive` | `gtest_cas_ref_writer.cpp` |
| `RefWriterRecovery` | `CasRefWriterRecovery` | `gtest_cas_ref_writer.cpp` |
| `RefWriterRecoveryRetry` | `CasRefWriterRecoveryRetry` | `gtest_cas_ref_writer.cpp` |
| `RefWriterRemount` | `CasRefWriterRemount` | `gtest_cas_ref_writer.cpp` |
| `RefWriterRuntimeIdentity` | `CasRefWriterRuntimeIdentity` | `gtest_cas_ref_writer.cpp` |
| `RefWriterSnapshotPublish` | `CasRefWriterSnapshotPublish` | `gtest_cas_ref_writer.cpp` |
| `RefWriterStalePrecommitSweep` | `CasRefWriterStalePrecommitSweep` | `gtest_cas_ref_writer.cpp` |
| `RefTableCacheEviction` | `CasRefTableCacheEviction` | `gtest_cas_ref_writer.cpp` |
| `RefWriterCarve` | `CasRefWriterCarve` | `gtest_cas_ref_carve.cpp` |
| `RefWriterChunkedFlush` | `CasRefWriterChunkedFlush` | `gtest_cas_ref_chunked_flush.cpp` |
| `RefWriterLaneExceptionSafety` | `CasRefWriterLaneExceptionSafety` | `gtest_cas_ref_lane_exception_safety.cpp` |
| `ContentAddressedLog` | `CasContentAddressedLog` | `gtest_cas_event_log.cpp` |
| `CatalogLifecycleReconciler` | `CasCatalogLifecycleReconciler` | `gtest_cas_gc_frontier_gate.cpp` |
| `CaLifecycle` | `CasLifecycle` | `gtest_ca_transaction.cpp` |
| `CaWiring` | **`CasTransactionWiring`** (see collision) | `gtest_ca_transaction.cpp` |
| `ContentAddressedSettings` | `CasContentAddressedSettings` | `gtest_cas_settings.cpp` (file renamed) |
| `INSTANTIATE_TEST_SUITE_P(InMemory, …)` | `CasInMemory` | `gtest_cas_backend_contract.cpp` |
| `INSTANTIATE_TEST_SUITE_P(Local, …)` | `CasLocal` | `gtest_cas_backend_contract.cpp` |
| `INSTANTIATE_TEST_SUITE_P(WinnerShape, …)` | `CasWinnerShape` | `gtest_cas_gc_frontier_gate.cpp` |

Not renamed, and their exclude entries kept: `CountingBackendShape`, `MemoryWriteBuffer`,
`TestCascadeWriteBufferWithDisk`. None of the three starts with `Cas`, so the exclude list and the
`Cas*` filter agree by construction — the generator now asserts that (an excluded name starting with
`Cas` is a fatal error, because the filter would run it anyway and the exclusion would be a fiction).

### Production symbols that share a spelling with a renamed suite

`ContentAddressedLog`, `ContentAddressedSettings` and `CatalogLifecycleReconciler` are **also
production class names**, in `src/Interpreters/ContentAddressedLog.{h,cpp}`,
`.../ContentAddressed/ContentAddressedSettings.{h,cpp}` and
`.../ContentAddressed/Gc/CatalogLifecycleReconciler.{h,cpp}`, referenced from `Context`, `SystemLog`,
`SystemLogBase`, `MetadataStorageFactory` and `ContentAddressedMetadataStorage`. A word-boundary
rename over the tree would have renamed the classes. Every substitution was anchored to
`^TEST(<name>,`, which cannot reach a type name, and the residual grep below confirms the class
references are intact.

### `CasWiring` collision — the brief's warning was correct

A **bare `CasWiring` suite already exists**: `src/Disks/tests/gtest_ca_wiring.cpp` defines
`TEST(CasWiring, TryFromDiskOnLocalDiskIsExceptionFreeAndCountsNoError)`. Renaming `CaWiring` →
`CasWiring` would have merged two unrelated files' tests into one gate entry (both are single-test
suites, so it would have compiled and passed — a silent identity merge).

Chosen instead: **`CasTransactionWiring`**, matching the `CasTransaction*` family that dominates
`gtest_ca_transaction.cpp` (`CasTransactionOps`, `CasTransactionLifecycle`, `CasTransactionRemove`, …).
Verified free: 0 exact hits across the union of both binaries' suite names.

`CaLifecycle` → `CasLifecycle` has **no** collision (0 exact hits; the only prefix neighbours are the
distinct `CasLifecycleCondition` and `CasLifecycleSnapshot`).

## Referrer sweep

Method: `git ls-files -z | xargs -0 grep -lIE '\b(<all 21 names>)\b'`, i.e. every tracked file,
untruncated. The first pass over the working directory was discarded as unusable — it was dominated by
`.claude/worktrees/`, `tmp/` and build-dir debris.

### Changed (executed / living)

| site | what |
| --- | --- |
| the 8 target files | the `TEST` macro suite names and 3 instantiation prefixes |
| `gtest_cas_part_write.cpp` | comment citing `RefWriterAppendLane.WedgedLaneBlocksSameTableWhileOtherTableProceeds` |
| `gtest_cas_request_control.cpp` | same test cited |
| `gtest_cas_ref_snapshot_publish_ordering.cpp` | comment citing `RefWriterAppendLane.CheckpointConflictAfterLogCommitRequiresRecoveryWithoutInstall` |
| `gtest_cas_ref_read_contract.cpp` | two comments: `RefTableCacheEviction`, and `RefWriterRuntimeIdentity.ColdReadRejectsReplacementByExternalPoolActor` |
| `gtest_cas_ref_writer.cpp` | two self-referencing comments naming its own suites |
| `.../ContentAddressed/README.md` | the `## Testing` recipe — a living how-to; rewritten to `Cas*` |
| `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md` | two sites: an **executable** `--gtest_filter` in Step 5 containing `RefTableCacheEviction*`, which would have silently selected nothing, and one pointer to "the `RefTableCacheEviction*` test family" |
| `docs/superpowers/cas/deferred-docs-fixes.md` | a pending-action entry pointing at "three `RefWriterRuntimeIdentity` tests"; identity substitution only, no claim rewritten |
| `utils/cas-gate/generate_cas_suites.sh` | rewritten as the verifier (second commit) |

The old README recipe was
`Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*`.
Two of its components matched **nothing** in either binary: `CA*` (no `CA`-prefixed suites exist) and
`RefSnapshotCodec*` (no such suite exists). The recipe had been carrying dead components — another
instance of a curated filter being unfalsifiable by reading it.

### Deliberately left as history

`docs/superpowers/{plans,specs,reports,worklogs}/…` (43 files), `docs/superpowers/cas/BACKLOG.md`,
`docs/superpowers/cas/2026-07-28-stage-a-RESULTS.md`,
`docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md`, `.superpowers/sdd/…` reports, and
`utils/ca-soak/scenarios/BACKLOG.md`. These record what was true when written. Four BACKLOG entries
(`BACKLOG.md` gate-gap sections, and the `Ca*`/`Cas*:CA*` filter post-mortems) exist *specifically* to
document the old names; rewriting them would erase the record of the gap this task closes.

Not changed because the hit is a **production symbol**, not a suite: `docs/superpowers/models/README.md`
(`CatalogLifecycleReconciler` the class), `docs/superpowers/cas/upstream-patch-inventory.md`
(`M(ContentAddressedLog, …)` the system-log registration), `docs/superpowers/cas/11-walkthrough.md`
(`ContentAddressedSettings.*` the source files), and every `src/` non-test file.

### Pre-existing stale references, outside this task's scope

`docs/superpowers/cas/BACKLOG.md` (the open CA-s3 lane item) and
`utils/ca-soak/scenarios/BACKLOG.md` both name `CaWiring*` in **live, actionable** items. Traced: they
cite `CaWiringGc.DroppedPartIsReclaimedByRounds` and
`CaWiringGc.DisplacedTreeBlobsReclaimedThroughRealPath` — the pre-rename spelling of the
`gtest_ca_wiring.cpp` family, which an **earlier** campaign already renamed to `CasWiring*`. They do
not point at the `CaWiring` suite this task renamed, and they were already dangling before this work.
Left untouched; flagged here as a docs-register item.

One imprecision noticed and left: the comment in `gtest_cas_ref_read_contract.cpp` describes
`CasRefTableCacheEviction` as "the production knob", but the knob is `ref_table_cache_bytes` — the name
is a test suite. Only the identity was substituted; rewriting the sentence was not in scope.

## The invariant, and its verification

`utils/cas-gate/generate_cas_suites.sh` no longer curates the CAS set. Three fatal checks:

1. a non-excluded CAS suite whose own name does not start with `Cas` — applied **before** the binary
   lookup, so a compile-guarded suite this build does not contain is still held to the invariant;
2. a parameterized suite whose `<Inst>/<Suite>` spelling does not start with `Cas`, i.e. a non-`Cas`
   instantiation prefix — `Cas*` matches the whole exposed spelling, so the prefix decides;
3. a CAS test file outside the source glob.

The source-vs-binary drift check (`unclaimed`) and the explicit `KNOWN_COMPILE_GUARDED` list are
unchanged; exit code on any failure is still 1.

**Check 3 decides CAS-ness by includes, not by suite names.** The first draft tested "does this file
define a `Cas`-prefixed suite", and the red-first run proved that insufficient: at HEAD it did **not**
flag `gtest_content_addressed_settings.cpp`, because that file was *both* misnamed *and* had a
non-`Cas` suite — exactly the combination that hid it from the gate. The predicate is now "does the
file include a header under `MetadataStorages/ContentAddressed/` or `cas_test_helpers.h`". Measured on
the tree: it flags that file, and produces **zero** hits across every other `gtest_*.cpp` under `src/`.

The CAS source glob is now a single `gtest_ca*.cpp`, which subsumes the old two-glob form. This is why
`gtest_content_addressed_settings.cpp` was renamed to `gtest_cas_settings.cpp` — the alternative was
hand-widening the glob, which is the same curation the task removes. The file is picked up by cmake's
`grep_gtest_sources` glob over `src/` (`src/CMakeLists.txt`), so the rename needed no build change; the
only tracked referrer to the old filename is a historical plan doc.

### Red-first evidence

Ran the new script against **HEAD's** sources (staged into a scratch tree from `git show HEAD:…`, 468
gtest files) with the **pre-rename** binary:

Run A — check 3:
```
error: 1 CAS test file(s) are not matched by gtest_ca*.cpp in src/Disks/tests:
  …/src/Disks/tests/gtest_content_addressed_settings.cpp
Rename each to gtest_ca*.cpp under src/Disks/tests so this script's enumeration sees it.
EXIT=1
```

Run B — checks 1 and 2, same tree with that file removed so the later checks are reached:
```
error: 23 CAS suite spelling(s) are not reachable by the plain 'Cas*' filter:
  CaLifecycle (suite name does not start with 'Cas')
  InMemory/CasBackendContract (INSTANTIATE_TEST_SUITE_P prefix does not start with 'Cas')
  Local/CasBackendContract (INSTANTIATE_TEST_SUITE_P prefix does not start with 'Cas')
  WinnerShape/CasGcCompletedRemovalFenceRace (INSTANTIATE_TEST_SUITE_P prefix does not start with 'Cas')
  CatalogLifecycleReconciler (suite name does not start with 'Cas')
  CaWiring (suite name does not start with 'Cas')
  ContentAddressedLog (suite name does not start with 'Cas')
  RefTableCacheEviction … RefWriterStalePrecommitSweep (16 more, all "does not start with 'Cas'")
Rename each so the spelling the binary exposes starts with 'Cas' -- that filter is the gate.
EXIT=1
```

All three checks fail on the pre-sweep tree and pass on the post-sweep tree, so each one fences
something.

### The payoff, measured

`--gtest_filter='Cas*'` and the generator's emitted list are the **same set** in both builds — not
"modulo documented differences", identical. The generator now asserts this itself and exits 1 on any
diff, so it cannot drift silently.

| | release | ASan |
| --- | --- | --- |
| generator suites / listed tests | 279 / 1997 | 297 / 2002 |
| `Cas*` suites / listed tests | 279 / 1997 | 297 / 2002 |
| in generator, not matched by `Cas*` | ∅ | ∅ |
| matched by `Cas*`, not in generator | ∅ | ∅ |

One-process release run, `build/cas_rename/plain_cas_filter_release.log`:
`1995 tests from 279 test suites ran. [ PASSED ] 1995`, plus `YOU HAVE 2 DISABLED TESTS`.
1995 + 2 = 1997, so the executed count reconciles exactly with the listed count; the two disabled ones
are the pre-existing `CasProtocol.DISABLED_RevalidateAbsentTreeDepRecreates` and
`CasProtocol.DISABLED_AdoptTreeOfReclaimedTreeFailsClosedAtAdoptTime`.

**Release vs ASan difference, explained by name.** 297 − 279 = 18 suites, all ASan-only, all in
`KNOWN_COMPILE_GUARDED`: the `Cas*DeathTest` suites absent from a release build by design. Zero
release-only suites. The 19th list entry, `CasRefInstallSafetyDeathTest`, is in **neither** binary and
is not a stale entry: it is gated on `MEMORY_TRACKER_DEBUG_CHECKS`, which needs `!NDEBUG`, and
sanitizer builds define `NDEBUG` — it appears only in a plain debug build. That is documented at the
`#if` in `gtest_cas_ref_install_safety.cpp`.

## Baseline vs after

No test count dropped. Both builds gained exactly the 6 previously-ungated
`CasContentAddressedSettings` tests.

| | release baseline | release after | ASan baseline | ASan after |
| --- | --- | --- | --- | --- |
| gated suites | 278 | 279 | 296 | 297 |
| gated tests (listed) | 1991 | 1997 | 1996 | 2002 |
| whole-binary suites | 723 | 723 | 734 | 734 |
| whole-binary tests | 21030 | 21030 | 13857 | 13857 |

The whole-binary totals are unchanged, which is what distinguishes a rename from a redefinition.
Mapping the baseline suite list through the rename table and diffing against the after list yields a
**single** line in each build, `> CasContentAddressedSettings` — every other suite is accounted for by
name, not by count.

### Full CA gate, both builds

| build | result | log |
| --- | --- | --- |
| release | `TOTALS: pass=279 fail=0 abort=0`, `GATE_EXIT=0` | `build/cas_rename/after_per_suite_release.txt`, `build/cas_rename/gate_release.log` |
| ASan (`SUITE_TIMEOUT=600`) | `TOTALS: pass=297 fail=0 abort=0`, `GATE_EXIT=0` | `build_asan/cas_rename/after_per_suite_asan.txt`, `build_asan/cas_rename/gate_asan.log` |

Zero `FAIL` and zero `ABORT` lines in either results file.

### Build markers

Both binaries were rebuilt from the post-sweep sources before any test run, and both markers were
checked before the results were trusted: `NINJA_EXIT=0` in `build/cas_rename/ninja_release.log` and in
`build_asan/cas_rename/ninja_asan.log`. After the commits, `ninja -n` reports no pending compile or
link work in either build directory, so HEAD is the tree that was tested. Every unit-test invocation
ran under `flock "$(git rev-parse --git-common-dir)/unit_tests.lock"`.

## Logs

- `build/cas_rename/`: `baseline_generate.log`, `baseline_suites_release.txt`,
  `baseline_counts_release.log`, `ninja_release.log`, `after_generate_release.log`,
  `after_suites_release.txt`, `after_counts_release.log`, `gate_release.log`,
  `after_per_suite_release.txt`, `plain_cas_filter_release.log`, `asan_only_suites.txt`, `count.py`
- `build_asan/cas_rename/`: `baseline_generate.log`, `baseline_suites_asan.txt`,
  `baseline_counts_asan.log`, `ninja_asan.log`, `after_generate_asan.log`, `after_suites_asan.txt`,
  `after_counts_asan.log`, `gate_asan.log`, `after_per_suite_asan.txt`

## Commits

| SHA | subject |
| --- | --- |
| `e977ab639a5` | `ca: tests -- Cas prefix …` — the `git mv` only |
| `3d959928e06` | `ca: tests -- Cas prefix …` — the renames and the referrer sweep |
| `c7a9b2c17bc` | `ca: gate -- the generator verifies the Cas-prefix invariant instead of curating coverage` |

The split across the first two is an accident, not a design: `git add` failed atomically on the
already-staged deleted path, so the first commit captured only the rename `git mv` had staged. Both
trees compile — at `e977ab639a5` the file is renamed with its content unchanged. Not amended, per the
branch rule. Each commit's contents were verified with `git show --stat HEAD`; files were staged by
explicit path only.

## Not finished / for someone else

- The two live BACKLOG items naming `CaWiringGc.*` are stale from an **earlier** rename (details
  above). They need the `CasWiringGc` spelling, but they are findings text in history-bearing files, so
  they belong in the docs register rather than in this sweep.
- `CasRefInstallSafetyDeathTest` has never been executed by either gate lane, because both lanes are
  builds that define `NDEBUG`. Covering it needs a plain debug build in the gate. Pre-existing; out of
  scope here.
- The `## Testing` bullet in `.../ContentAddressed/README.md` now claims `Cas*` runs the whole set. It
  is true for both gated builds and asserted by the generator, but it does not mention that the
  `Cas*DeathTest` suites are absent from a release binary; the generator's `KNOWN_COMPILE_GUARDED`
  list is where that is recorded.
