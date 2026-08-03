# Tidy-fix draft report (T8, CAS scope)

Worktree: `/home/mfilimonov/workspace/ClickHouse/draft-tidy`, branch `draft/tidy-fixes`, base `7e20be96be9`.
Commit: `b0f87e8aaf1` — "ca: draft — clang-tidy fixes for CAS scope (UNVERIFIED-DRAFT, no runs)".

No builds, ninja, clang-tidy invocations, or test runs were performed in this worktree — write-only,
per the hard constraint. Every fix below is unverified until the finisher runs clang-tidy and the
affected suites.

## Coverage horizon

The tidy log (`build_amd_tidy/t8_tidy_build2.log`, read from the main worktree) had progressed to
`[4699/5386]` with no `T8_TIDY2_DONE` marker when diagnostic extraction began. By the time this report
was written the log had advanced further and the `T8_TIDY2_DONE` marker was present (last progress line
`[5356/5386]`). I re-extracted the full CAS-scoped diagnostic set against the finished log and diffed it
against the set I had already fixed: **zero diagnostics differ** — the same 119 unique CAS-scoped sites
appear in both the partial and the finished log. No delta remains for the finisher to pick up from a
later horizon; the finisher's clang-tidy re-run is purely a verification step, not a discovery step.

## Method

`grep -a "error:" <log>` filtered to paths under `ContentAddressed/`, `gtest_ca*`/`gtest_cas*`/`cas_*`,
deduplicated by `file:line:message` (header diagnostics that repeat once per including TU collapse to
one row). 327 total tidy errors in the log; 311 raw matches under the CAS path filter; 119 unique sites
after dedup, spanning 12 production files and 30 test files.

## Per-class totals

| Check | Sites | Disposition |
|---|---|---|
| `bugprone-suspicious-stringview-data-usage` | 4 | 3 fixed (size-aware rewrite), 1 NOLINT (bounded read, no NUL needed) |
| `modernize-raw-string-literal` | 53 | 51 converted to raw strings, 2 NOLINT (mix `\"` with `\n`, can't raw-string) |
| `cppcoreguidelines-pro-type-member-init` / `hicpp-member-init` | 2 | both fixed (default member initializers) |
| `hicpp-exception-baseclass` | 9 (3 types × 3 throw sites each, counted as 3 unique struct defs) | fixed (derive from `std::exception`) |
| `bugprone-empty-catch` | 8 | all NOLINT'd (all are deliberate best-effort/expected-exception swallows, all already or now commented) |
| `bugprone-parent-virtual-call` | 6 | fixed: 5 routed to the immediate parent, 1 revealed a real defect (see CODE findings) |
| `clang-analyzer-deadcode.DeadStores` | 4 | all NOLINT'd (false positives — value is read later, past the analyzer's local view) |
| `bugprone-argument-comment` | 4 | fixed (stale `epoch`/`gen` comments corrected to the real parameter names `our_epoch`/`admitted_generation`) |
| `cppcoreguidelines-init-variables` | 4 | fixed (explicit fail-closed initializer) |
| `performance-unnecessary-copy-initialization` | 4 | 3 fixed (const ref, container not mutated in scope), 1 NOLINT (must stay an independent copy — see below) |
| `performance-no-automatic-move` | 3 | fixed (dropped `const` on the moved-from binding) |
| `readability-container-size-empty` | 2 | fixed |
| `google-default-arguments` | 3 | fixed (dropped defaults from 3 virtual overrides; no call site in the file relies on them) |
| `readability-make-member-function-const` | 5 | fixed (`const` added; all route through `shared_ptr` members) |
| `bugprone-optional-value-conversion` | 1 | fixed (real minor bug, see CODE findings) |
| `readability-container-contains` | 1 | fixed |
| `performance-move-const-arg` | 1 | fixed (dropped `const`) |
| `misc-unused-using-decls` | 1 | NOLINT (used only inside `#ifndef DEBUG_OR_SANITIZER_BUILD`; unused in this TU's config, used in the other) |
| `hicpp-invalid-access-moved` | 1 | NOLINT (added to an existing `bugprone-use-after-move` NOLINT list; deliberate moved-from access) |
| `clang-analyzer-optin.core.EnumCastOutOfRange` | 1 | NOLINT (test constructs the impossible value on purpose) |
| `cert-msc32-c` / `cert-msc51-cpp` | 1 | NOLINT (deterministic fuzz seed, matches existing repo idiom) |
| `bugprone-switch-missing-default-case` | 1 | fixed (added `default:` arm) |
| `bugprone-suspicious-missing-comma` | 1 | NOLINT (deliberate adjacent-literal concatenation testing a split UTF-8 sequence) |

## CODE findings (real defects, not noise)

1. **`PutHookBackend::casPut`** (`src/Disks/tests/gtest_cas_ref_recovery_cas_walk.cpp`, was line 206)
   called `CountingBackend::casPut` directly, reaching past its immediate parent `HidingListBackend`.
   `HidingListBackend::casPut` implements the fixture's fault injection (`before_cas_put` hook and
   `ambiguous_cas_count`/`Poco::TimeoutException` simulation); `PutHookBackend` is designed to layer its
   own `fireIfWatched` hook *on top of* that, but the grandparent-skipping call silently disabled
   `HidingListBackend`'s fault injection whenever a test constructed a `PutHookBackend`. Currently inert:
   neither of the two tests that construct a `PutHookBackend` sets `before_cas_put` or
   `ambiguous_cas_count`, so nothing observably changed — but the bug is real and would silently break
   any future test that tries to compose both behaviors. Fixed by routing through
   `HidingListBackend::casPut`.

2. **`PostFoldUnreadableTerminalBackend::existsIgnoringFault`** (`gtest_cas_gc_frontier_gate.cpp`) and
   **`CatalogAfterListBackend::list`**'s internal catalog probe (`gtest_cas_namespace_janitor.cpp`) both
   reached past their immediate parent `CountingBackend` straight to `InMemoryBackend`, skipping
   `CountingBackend`'s head/get counters for those calls. Verified no assertion in either test depends on
   those counters, so this one is a `bugprone-parent-virtual-call` true-negative for behavior (no
   observable difference) but still a latent trap: routed both through `CountingBackend` instead, which
   is what the check recommends and costs nothing.

3. **`gtest_cas_orphan_manifest_sweep.cpp`**: `*before.token` dereferenced a
   `std::optional<Token>` unconditionally before passing it to `casPut(..., const std::optional<Token> &
   expected)`, which just wraps it back into an optional — an unnecessary and UB-risking dereference with
   zero benefit. Fixed by passing `before.token` directly.

## Deliberate false-positive suppressions worth flagging to the finisher

- `gtest_cas_ref_statemachine.cpp` (`E3AdmitsPreviewLeavesStateByteIdentical`): tidy's
  `performance-unnecessary-copy-initialization` suggested turning `const RefTableState before = state;`
  into a reference. That would be a real bug — `state` is mutated later in the same test, and the whole
  point of `before` is to capture its value *before* that mutation for a later byte-identical comparison.
  NOLINT'd with an explicit comment; **do not "fix" this one**.
- 4 `clang-analyzer-deadcode.DeadStores` sites (`plans_before`, `lost_before`, `remounts_before`,
  `puts_before`) are all read later in the same test, just past the analyzer's flow-sensitive window
  (typically across a `.join()`/thread boundary). Matches an existing repo precedent
  (`src/Processors/Formats/Impl/Parquet/ReadManager.cpp`) for the same bare-NOLINT idiom.

## Finisher checklist

1. Re-extract the complete CAS-scoped diagnostic list once more against the finished
   `build_amd_tidy/t8_tidy_build2.log` (or a fresh run) and diff against
   `/tmp/.../scratchpad/cas_errors_final.txt`-equivalent — I already did this once post-hoc and found zero
   delta, but re-verify against whatever log the finisher actually gates on.
2. Run `clang-tidy -p build_amd_tidy <file>` for every touched file (12 production + 30 test files, listed
   in `git show --stat b0f87e8aaf1`) and confirm zero CAS diagnostics remain.
3. Targeted suite runs, release and ASan:
   - `CasFormatBattery`, `CasRefSnapshotCodec`, `CasRefEpochSealFormat`, `CasGcHoldGrammar`,
     `CasRefChunkPreparation`, `CasRefDecodeBounds` and friends (the raw-string-literal conversions touch
     golden/needle byte literals in these; the conversion script only ever removed backslash-escaping
     around `"` — no byte content changed — but these are exactly the tests where a silent content change
     would matter most).
   - `CasGcFrontierGate`, `CasNamespaceJanitor`, `CasRebuildCondemnNothing`, `CasRefRecoveryCasWalk` (the
     parent-virtual-call fixes, including the one real `PutHookBackend` defect).
   - `CasMountLease`, `CasServerRootClaim`, `CasMount` (const/member-init/argument-comment fixes).
   - `CasParallelCommit` (5 methods made `const`).
   - Full `Cas*:CA*` gtest gate filter as a blanket sweep.
4. Double-check the `PutHookBackend::casPut` fix in isolation: add (or confirm someone adds) a test that
   sets `before_cas_put`/`ambiguous_cas_count` on a `PutHookBackend` instance to prove the composed
   behavior now actually fires — today's tests don't exercise it either way.
