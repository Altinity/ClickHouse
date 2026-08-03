# Task 7 — `clickhouse-disks`: `ca-*` commands → `cas-*`, deprecated `fsck` alias dropped

## Enumeration

`git ls-files | xargs grep -l 'ca-fsck|ca-gc-dryrun|ca-gc-rebuild|ca-inspect|ca-drop-member'` minus
`docs/superpowers/`, `.superpowers/`, `contrib/`: **41 files**.

**Deviation (widening):** the plan's file list names only `programs/disks/`, two integration tests and
`utils/ca-soak/`. Fifteen of the 41 are elsewhere — ten under
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (including a user-visible error
message in `ContentAddressedMetadataStorage.cpp` telling the operator to run `cas-fsck`, the tool's own
exception messages in `CasInspect.cpp`, and that subtree's `README.md`), `InterpreterSystemQuery.cpp`,
plus `tests/integration/test_cas_ref_snaplog/configs/storage_conf.xml`. Every hit was inspected before
the sed: all are the command name in prose, in an error/help string, or in an invocation — none is a
disk name or a path segment. The sed was run over the full 41.

## Applied

- The five command strings renamed to `cas-fsck`, `cas-gc-dryrun`, `cas-gc-rebuild`, `cas-inspect`,
  `cas-drop-member`.
- `CommandFsckDeprecated` deleted: the class, its `makeCommandFsckDeprecated` factory, the declaration
  in `ICommand.h`, and the `command_descriptions.emplace("fsck", …)` registration in `DisksApp.cpp`.
- `CommandFsck`'s own `description` lost its trailing "(`fsck` is a deprecated alias for this
  command.)" — that sentence would otherwise advertise a command that no longer exists.
- C++ class/factory names (`CommandCaInspect`, `makeCommandCaGcDryRun`, …) kept, per the plan.
- Generic prose uses of the word "fsck" in internal comments were left: `FSCK` is on the keep list, and
  those sentences name the concept, not the command.

## Verification

- `grep -n 'ca-fsck|ca-gc-dryrun|ca-gc-rebuild|ca-inspect|ca-drop-member|FsckDeprecated'` tree-wide
  (minus the excluded dirs): **empty**.
- `ninja -C build clickhouse unit_tests_dbms` → `NINJA_EXIT=0` (`build/obscure_t7_build.log`).
- **Live check against the built binary** (`clickhouse disks --query 'help'` over a throwaway local
  disk config): the command list shows `cas-drop-member`, `cas-inspect`, `cas-gc-rebuild`,
  `cas-gc-dryrun`, `cas-fsck`, and no `fsck` entry; `cas-fsck`'s help text no longer mentions an alias.
  `--query 'fsck'` now exits 36 and prints the available-commands listing, i.e. the alias is really gone
  rather than merely unlisted.
- `python3 -m pytest tests scenarios/tests` in `utils/ca-soak`: **336 passed** (six ca-soak files that
  invoke these commands are in the changed set).
