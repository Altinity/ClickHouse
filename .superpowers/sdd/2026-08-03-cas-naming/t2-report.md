# Task 2 report — SQL commands and grants, `CONTENT ADDRESSED` -> `CAS`

## Step 1 — enum constants, access types, AST field
- `ASTSystemQuery.h`: the 7 `CONTENT_ADDRESSED_*` `Type` constants are now `CAS_GC_RUN`, `CAS_GC_REBUILD`,
  `CAS_DROP_POOL_MEMBER`, `CAS_FSCK`, `CAS_FORGET`, `CAS_GC_STOP`, `CAS_GC_START`. Applied across 9 files
  carrying `CONTENT_ADDRESSED_` and 2 carrying `SYSTEM_CONTENT_ADDRESSED_`.
- AST field `content_addressed_gc_rebuild_force` -> `cas_gc_rebuild_force`, all 4 uses
  (`ASTSystemQuery.h`, `ASTSystemQuery.cpp`, `ParserSystemQuery.cpp`, `InterpreterSystemQuery.cpp`).
- **The plan's "fix `AccessType.h` alias strings by hand" turned out to be unnecessary**: the
  `SYSTEM_CONTENT_ADDRESSED_` -> `SYSTEM_CAS_` sed fixed the enum names and Step 2's phrase sed fixed the
  alias literals, landing exactly the rows the plan specified. Verified by reading all 7 rows, not assumed.
- **Why the rename is atomic across parse and format:** `getTypeIndexToTypeName` builds the command text
  from `magic_enum::enum_entries` with `_` replaced by space, so the constant IS the SQL text. Confirmed by
  reading the function rather than inferring it from the plan's claim.

## Step 2 — SQL text in strings, comments, tests, soak
`CONTENT ADDRESSED` -> `CAS` across **43** files (`src/`, `tests/queries/`, `tests/integration/`,
`utils/ca-soak/`). Tree-wide re-grep for the phrase outside `docs/` returns nothing.

Diff reviewed for prose collateral: every changed site is SQL command text — exception/log messages,
`ProfileEvents.cpp` descriptions (`CASGcRebuildVirginByEnumeration`, `CASIdentityLost`,
`CASDataRootVanished` all name `SYSTEM CAS …` in their recovery advice), gtest expectations,
`framework/gc.py`'s `GC_SQL`, and scenario cards/READMEs. No English prose was rewritten.

## Step 3 — privileges reference
Already correct after Step 2 (the reference file was among the 43). Not hand-edited. Verified
**against the built binary**, not by eye: `clickhouse local --query "SHOW PRIVILEGES"` filtered to CAS
rows is byte-identical to `01271_show_privileges.reference:161-167`, tabs included (compared under `cat -A`).

## Step 4 — verification
### Residual `CONTENT_ADDRESSED` hits, each classified
- `ContentAddressedSettings.{h,cpp}`: `LIST_OF_CONTENT_ADDRESSED_SETTINGS`,
  `CONTENT_ADDRESSED_SETTINGS_SUPPORTED_TYPES` — internal C++ macros, kept by the Global Constraints.
- `tests/clickhouse-test`, `tests/config/install.sh` — the tag and runner flags, explicitly **Task 5's** scope.
- Three comments that the rename made false, fixed here rather than deferred, because each pointed at
  constants that no longer exist under that name: `ASTSystemQuery.cpp` and `ParserSystemQuery.cpp`
  ("the sibling `CONTENT_ADDRESSED_*` commands' … disk target" -> `CAS_*`), and `CasRequestControl.h`
  (a suggested future error code `CONTENT_ADDRESSED_WRITE_RETRY_LATER` -> `CAS_WRITE_RETRY_LATER`).

### Build and tests
- `build/naming_t2_build.log` -> `NINJA_EXIT=0`. Two of the comment fixes landed *after* that build started,
  so it was rebuilt: `build/naming_t2_build2.log` -> `NINJA_EXIT=0` covers the final tree. A green build on a
  stale tree is evidence about a different binary.
- `build/naming_t2_unit.log`: `--gtest_filter='*Parser*:Cas*'` under `flock` — **2124 tests from 310 suites,
  all PASSED**, exit 0.

### The tests are not vacuous — behaviour was checked, not just compilation
`gtest_Parser.cpp` asserts the literal round-trip of `SYSTEM CAS DROP POOL MEMBER 'srv1' FROM DISK 'disk1'`
and `SYSTEM CAS GC RUN …` (11 assertions), and 11 more `SYSTEM CAS` expectations live in five
`src/Disks/tests/gtest_cas_*.cpp` files. End-to-end against the built binary:

- `SYSTEM CONTENT ADDRESSED GC RUN foo` -> `Code: 62 … Syntax error: failed at position 16 (ADDRESSED)`.
  The old spelling no longer parses — this is the evidence that **no back-compat alias survived**, which the
  spec requires and which no gtest asserts.
- `SYSTEM CAS GC RUN foo` -> `Code: 479 … Unknown disk foo`. The keyword parses and reaches disk resolution.

## Step 5 — commit
Staged by explicit file list (`utils/ca-soak/` and `tests/integration/` carry untracked run debris, so
`git add -A` on those roots is unsafe here); `.superpowers/sdd/task-5-report.md`, a foreign pre-existing
modification, stays unstaged.
