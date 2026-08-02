# T1c commit 1: fixture seam

## Scope actually done

Introduced ONE named test-only entry point, `DB::Cas::tests::fixture`, in
`src/Disks/tests/cas_test_helpers.h`. No behavior migration, no deletion of `stageATransition`. Pure
addition plus a small demonstration conversion, as scoped.

## Usage-pattern survey

Grepping `stageATransition` across `src/Disks/tests/` (287 occurrences, 37 test files plus
`cas_test_helpers.h` itself) turned up exactly three distinct usage shapes:

| Pattern | What it does | Existing call shape | Seam replacement |
|---|---|---|---|
| Identity-only | Derive the deterministic fixture life for a namespace, then use it directly (as a key input, a comparison, or to call `layout.*Key(life, ...)`) | `NamespaceLifeId::stageATransition(ns)` | `fixture::fixtureLife(ns)` |
| Admit-Live-without-`_ckpt` | Admit a namespace into the catalog as a bare `Live` entry with no `_ckpt`, idempotently | `casAdmitEntry(backend, layout, ns)` (internally calls `stageATransition`) | `fixture::admitLive(backend, layout, ns)` |
| Raw ref-log write | Write a `RefLogTxn` object directly under `_log/`, admitting the namespace first and resolving to whichever life is already on record (real or sentinel) | `writeRefLogTxnRaw(backend, layout, txn)` | `fixture::writeRefLogRaw(backend, layout, txn)` |

Migration recipe for commit 2 (mechanical, one pattern at a time):
- Every direct call `NamespaceLifeId::stageATransition(ns)` (or `DB::Cas::NamespaceLifeId::stageATransition(ns)`) becomes `fixture::fixtureLife(ns)`.
- Every call to the existing `casAdmitEntry` helper becomes `fixture::admitLive`.
- Every call to the existing `writeRefLogTxnRaw` helper becomes `fixture::writeRefLogRaw`.

`casAdmitEntry` and `writeRefLogTxnRaw` themselves are UNCHANGED apart from swapping their own internal
`stageATransition` call for `fixture::fixtureLife` — they still exist under their original names, so
commit 2's sweep can proceed call-site-by-call-site without a flag day. Whether to eventually fold the
original names away entirely (so `casAdmitEntry`/`writeRefLogTxnRaw` become private details only the
`fixture` wrappers call) is commit 2's call, not this commit's.

No other usage shape exists in the test tree: `registerNamespaceRaw` (a no-op forward-declared helper),
`writeRefSnapshotRaw`, and the checkpoint-fixture family (`writeRecoverableCkptForRawFixture`,
`casAdmitRecoverableEntry`, etc.) all resolve life through `CasRefCatalog::resolveLifeOrSentinel` /
`lifeIfCataloged` against an ALREADY-admitted catalog entry — they never call `stageATransition`
themselves, so they are out of this seam's scope (they read the identity `admitLive` already minted,
they don't mint it).

## Seam API

In `src/Disks/tests/cas_test_helpers.h`, `namespace DB::Cas::tests::fixture`:

- `NamespaceLifeId fixtureLife(const RootNamespace & ns)` — delegates to
  `NamespaceLifeId::stageATransition(ns)`. Defined right before its first use (`seedFoldCursorForTest`).
- `void admitLive(Backend & backend, const Layout & layout, const RootNamespace & ns)` — delegates to
  `casAdmitEntry`. Defined immediately after `casAdmitEntry`'s definition.
- `void writeRefLogRaw(Backend & backend, const Layout & layout, const RefLogTxn & txn)` — delegates to
  `writeRefLogTxnRaw`. Defined immediately after `writeRefLogTxnRaw`'s definition.

The doc comment on the `fixture` namespace (attached at `fixtureLife`, since all three divergences are
one seam) enumerates the three deliberate divergences from production this test tree relies on:
1. `fixtureLife` is a deterministic namespace-derived identity, never a fresh random mint.
2. `admitLive` reaches `Live` with no `_ckpt`, whereas production only reaches `Live` through
   `completeCreation`, which publishes `_ckpt` first.
3. `writeRefLogRaw` writes ref-log bytes directly at the resolved fixture identity, bypassing the
   writer's birth/append lane.

No task ids, plan, or review references are in the new comment (comment policy). The PRE-EXISTING doc
comments on `casAdmitEntry` and `writeRefLogTxnRaw` (which do reference "Task 4-C"/"Task 6"/"the Task 4-B
map") were left untouched — cleaning those is a prose sweep outside this commit's scope, not something
introduced here.

## Demo conversions (3 inside `cas_test_helpers.h`, 1 test file)

- `seedFoldCursorForTest`: two internal `NamespaceLifeId::stageATransition(ns)` calls → `fixture::fixtureLife(ns)`.
- `casAdmitEntry`: its internal `NamespaceLifeId::stageATransition(ns).incarnation` → `fixture::fixtureLife(ns).incarnation`.
- `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp`, `CasNamespaceFileReadContract.ListThroughHeldLifeIssuesZeroCatalogRequests`:
  `NamespaceLifeId::stageATransition(RootNamespace{...})` → `fixture::fixtureLife(RootNamespace{...})`
  (resolves via the file's existing `using namespace DB::Cas::tests;`).

## Verification

```
flock "$(git rev-parse --git-common-dir)/unit_tests.lock" bash -lc \
  'ninja -C build unit_tests_dbms > build/t1c1_build.log 2>&1 && \
   build/src/unit_tests_dbms --gtest_filter="CasNamespaceFileReadContract.*:CasRefReadContract.*:CasRefCatalog.*" > build/t1c1_run.log 2>&1'
```

Build: clean (114/114 targets, no warnings from the touched files).
Run: `[==========] 18 tests from 3 test suites ran. [ PASSED ] 18 tests.` — includes all 3
`CasNamespaceFileReadContract` tests (the converted one passes), all 3 `CasRefReadContract` tests, and
all 12 `CasRefCatalog` tests. Identical pass count/behavior to before the change (pure addition + demo
conversion, no logic changed).

## Deviations from the brief

None. Commit scoped exactly to the seam + demo conversions; no tree-wide migration attempted.
