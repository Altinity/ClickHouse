# CAS mount-lease abort hardening + operator-driven disk lifecycle (UNMOUNT / FSCK) design

Status: user-approved design (2026-07-21).

## Context {#context}

CI PR #2073 (`Stateless tests (amd_debug, sequential)`) crashed with a `LOGICAL_ERROR`
(`CAS mount-lease: key '…' was touched by a foreign writer — failing closed, never re-minting`,
STID 3982-3b48) that aborted the server and failed unrelated tests as collateral. Root cause chain,
confirmed from the server log + code:

1. A custom CAS disk created via the inline `disk(...)` AST function is cached forever in the
   `Context` disk selector (`Context::getOrCreateDisk`, no teardown). Its `CasPool` background threads
   (`ContentAddressedGC` scheduler + `MountLeaseKeeper` renewal) keep running after every table using
   the disk is dropped — `Pool::~Pool` (the clean-farewell teardown) never runs because the disk is
   never destroyed.
2. The `04295_content_addressed_mutation_no_leftovers.sh` "no-leftovers" test drops the table, polls
   the pool dir until GC empties it, then `rm -rf`s the pool dir. That deletes the backing store out
   from under the still-running threads.
3. The `MountLeaseKeeper` renewal thread's `renewOnce` then does `putOverwrite(mountKey, …)` which
   fails (backing object gone). `MountLeaseKeeper::onRenewMismatch` re-reads the key, finds it
   **absent**, cannot classify (no foreign writer — the store is simply gone), and falls through to
   the base `SingleWriterSlot::onRenewMismatch` which throws `LOGICAL_ERROR`. In a debug/ASan build
   the `LOGICAL_ERROR` exception constructor aborts the process (`abort_on_logical_error`). In a
   release build `backgroundLoop`'s `catch(...)` swallows it and logs — no crash. So the crash is a
   debug/ASan artifact of mis-classifying an environmental condition (backing store deleted) as a
   logic-invariant violation.

Two problems fall out. This spec addresses both, at the layers the user chose:

- **The abort (Part 1):** a background renewal thread must not abort the server when its backing
  store vanishes — that is an environmental condition, not a logic error.
- **The lifecycle (Parts 2–4):** custom CAS disks are never torn down, so after a test's `rm -rf`
  they spin forever in "broken" GC retries, and a later test that reuses the same disk finds it in a
  broken state. Rather than auto-teardown on `DROP` (which would couple generic `Context`/`DROP`
  code), give operators/tests **explicit** SYSTEM commands to unmount a CAS disk cleanly and to fsck
  it online, and wire them into the "no-leftovers" test teardown.

The lifecycle fix is intentionally operator/test-driven, not automatic-on-DROP: the disk-leak on
`DROP` (the `Context` disk selector never evicting) stays out of scope, but `UNMOUNT` gives a clean
manual/test path and the abort-hardening makes even a stray `rm -rf` non-fatal.

Key building blocks that already exist and are reused:

- `ContentAddressedMetadataStorage::shutdown()` — stops the GC scheduler and releases the pool
  (`cas_store.reset()`), which runs `Pool`'s clean-farewell teardown and joins the background threads.
- `ContentAddressedMetadataStorage::startup()` — opens the pool; guarded only by `if (cas_store)
  return;`, so it is re-runnable once `cas_store` is null.
- `runFsck(Pool & store, bool detail, …)` (`Tools/CasFsck.h`) — a read-only fsck scan taking a live
  pool; usable online with no quiescing.
- The `SYSTEM CONTENT ADDRESSED *` verb family (`CONTENT_ADDRESSED_GC_RUN`,
  `CONTENT_ADDRESSED_GC_REBUILD`, `CONTENT_ADDRESSED_DROP_POOL_MEMBER`) — already wired through
  `ASTSystemQuery` (with a `disk` field), `InterpreterSystemQuery` (dispatch + access), and
  `AccessType`. New sibling verbs extend it consistently; the CA-specific coupling in generic system
  code already exists for this family, so no *new* coupling is introduced.

## Part 1 — abort-hardening + `CasMountLeaseLost` counter {#part1}

**File:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
(and `src/Common/ProfileEvents.cpp` for the new event).

In `MountLeaseKeeper::onRenewMismatch`, add an explicit branch for the vanished-backing-store case
**before** the fall-through to `SingleWriterSlot::onRenewMismatch`:

```cpp
if (!got)
{
    /// The mount slot object VANISHED (backing store deleted under a live mount -- e.g. an operator
    /// or test rm -rf'd the pool dir). This is an ENVIRONMENTAL condition, not a logic error: there
    /// is no foreign writer to fail closed against (the store is simply gone). Stop renewing
    /// (fail-closed: the write fence latches to lost, we never re-mint) WITHOUT aborting the server --
    /// throwing LOGICAL_ERROR here would abort debug/ASan builds via abort_on_logical_error.
    ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
    emitMountEvent(event_sink, CasEventType::MountConflict, srid, "vanished", nullptr,
        "mount slot object vanished (backing store deleted under a live mount) -- stopping renewal, fail-closed");
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
        "CAS mount-lease: key '{}' vanished (backing store deleted under a live mount) -- "
        "stopping renewal, fail-closed (never re-minting)", mismatched_key);
}
SingleWriterSlot::onRenewMismatch(mismatched_key);
```

- Add `extern const int FILE_DOESNT_EXIST;` to this TU's `ErrorCodes` block.
- `ProfileEvents::increment(ProfileEvents::CasMountLeaseLost)` is added at each **non-recoverable**
  terminal branch: the new `vanished` branch here, plus the existing `superseded` and
  `foreign_writer` branches in `MountLeaseKeeper::onRenewMismatch`. It is **not** added to the
  recoverable `fenced_by_gc` branch (a GC-fence-then-reopen is normal under GC races and would be
  noise).
- New ProfileEvent in `ProfileEvents.cpp`: `CasMountLeaseLost` — description in the file's
  operator-facing house style, e.g. *"Counts CAS mount-lease terminal losses (the backing store
  vanished, or the slot was superseded / taken by a foreign server); the keeper stopped renewing and
  latched its write fence to lost. Non-zero indicates a mount was lost -- investigate via
  system.content_addressed_log MountConflict rows."*

**Invariant preserved:** the only behavioural change is *no abort*. `renewOnce` already sets
`last_renew_failure_was_confirmed_mismatch = true` before calling the hook, so `backgroundLoop`
treats the throw as a confirmed terminal failure → stops the loop → `onRenewFailed()` latches the
write fence to lost → no re-mint. Genuine `superseded` / `foreign_writer` stay fatal `LOGICAL_ERROR`
(real single-writer violations, fail-hard justified). In release builds nothing changes.

## Part 2 — `SYSTEM CONTENT ADDRESSED UNMOUNT <disk>` + auto-remount {#part2}

**Files:** `src/Parsers/ASTSystemQuery.h` (+`.cpp` for formatting), `src/Parsers/ParserSystemQuery.cpp`,
`src/Access/Common/AccessType.h`, `src/Interpreters/InterpreterSystemQuery.cpp` (+`.h`),
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}`.

**Command:** `SYSTEM CONTENT ADDRESSED UNMOUNT <disk>` (new `Type::CONTENT_ADDRESSED_UNMOUNT`,
`AccessType::SYSTEM_CONTENT_ADDRESSED_UNMOUNT`), modelled on the `CONTENT_ADDRESSED_GC_RUN` wiring
(parse `<disk>` into the existing `disk` field; dispatch + `required_access`).

**Handler:** resolve the disk by name, obtain its `ContentAddressedMetadataStorage`, and call
`shutdown()`. Effect: stop the GC scheduler + `MountLeaseKeeper` renewal thread, run `Pool`'s clean
farewell, `cas_store.reset()` → the disk is **dormant**. After this a subsequent `rm -rf` of the pool
dir is safe (no live threads → no broken GC retries, no mount-lease abort). If the disk is not a CAS
disk, or the name is unknown, the handler throws a clear `BAD_ARGUMENTS`.

**Auto-remount on next access:**
- `ContentAddressedMetadataStorage::shutdown()` currently sets `shutdown_called = true`. Change it so
  a subsequent `startup()` can re-open: clear the started/`shutdown_called` latch so re-mount is
  clean (or gate startup only on `cas_store` being null — `startup()` already does `if (cas_store)
  return;`, so the change is to ensure `shutdown_called` does not permanently block operations).
- Add a private `ensureMounted()` on the metadata storage: under `pointer_mutex`, `if (!cas_store)
  startup();`. Call it at the single choke-point `store()` (and any operation entry that bypasses
  `store()`), so the next table access after an `UNMOUNT` transparently re-mounts. After an
  `UNMOUNT` + `rm -rf`, the re-mount opens a fresh empty pool (`createOrValidate` mints fresh pool
  meta) — exactly the desired clean-reuse behaviour.
- Thread-safety: `startup()` is `TSA_NO_THREAD_SAFETY_ANALYSIS` and documented single-threaded;
  `ensureMounted()` serialises the lazy re-open under `pointer_mutex`, and concurrent `store()`
  callers either observe the freshly-set `cas_store` or block on the mutex during re-open.

**Guardrail note (documented, not enforced):** `UNMOUNT` is intended for a disk with no live tables
(the "no-leftovers" test drops its table first). Auto-remount makes reuse safe; a stray `UNMOUNT`
while a table is live is not defended against beyond auto-remount-on-next-access, and is out of
scope.

## Part 3 — `SYSTEM CONTENT ADDRESSED FSCK <disk>` (online) {#part3}

**Files:** same generic-system set as Part 2, plus the handler in `InterpreterSystemQuery.cpp`.

**Command:** `SYSTEM CONTENT ADDRESSED FSCK <disk>` (new `Type::CONTENT_ADDRESSED_FSCK`,
`AccessType::SYSTEM_CONTENT_ADDRESSED_FSCK`).

**Handler:** resolve the CAS disk, obtain its live pool via `store()`, call `runFsck(*store,
/*detail=*/…)` (read-only, online — no quiescing). Return the `FsckReport` as a **result set** (the
`CONTENT_ADDRESSED_*` handlers already return `BlockIO`): one row per examined object with its class
(`reachable` / `unreachable` / `orphaned-unaccounted` / `corrupt`) and size, or a compact summary
form. A test asserts "0 unreachable / 0 orphaned / 0 corrupt" before teardown. Draining before the
scan uses the existing `SYSTEM CONTENT ADDRESSED GC RUN <disk>`.

Exact result-set shape (columns) is fixed during implementation to match `FsckReport`/`FsckObject`;
prefer the same columns the offline `clickhouse-disks fsck` applet prints so operators see one shape.

## Part 4 — test integration {#part4}

**Files:** `tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.sh` and the
sibling `04290_content_addressed_no_leftovers.*` (and any other "no-leftovers" family member).

Replace the teardown `poll pool dir empty → rm -rf` pattern with:

```
SYSTEM CONTENT ADDRESSED GC RUN <disk>   (×N until drained)
SYSTEM CONTENT ADDRESSED FSCK <disk>     (assert 0 unreachable/orphaned/corrupt)
SYSTEM CONTENT ADDRESSED UNMOUNT <disk>  (stop threads, clean farewell, dormant)
rm -rf "${POOL_DIR}"                      (now safe: no live threads)
```

Setup is unchanged — the first table access after a prior `UNMOUNT` auto-remounts a fresh pool.

## Part 5 — rename the offline fsck applet `fsck` → `ca-fsck` {#part5}

**Files:** `programs/disks/CommandFsck.cpp` (and its registration in the disks command registry;
optionally rename the file to `CommandCaFsck.cpp`), plus any docs/tests invoking `clickhouse-disks
fsck`.

The offline `clickhouse-disks` CA applets are all `ca-`-prefixed (`ca-gc-dryrun`, `ca-gc-rebuild`,
`ca-inspect`); `fsck` is the lone outlier. Rename `command_name = "fsck"` → `"ca-fsck"` for
consistency (and to avoid implying a generic filesystem-check). This is a minor bundled cleanup ("при
случае"), independent of the online `SYSTEM CONTENT ADDRESSED FSCK` verb in Part 3; do it in the same
change so the two fsck surfaces (offline `ca-fsck`, online `SYSTEM CONTENT ADDRESSED FSCK`) land with
one consistent name family. Update the `08-testing-and-soak.md` §1.2 reference to `ca-fsck`.

## Testing {#testing}

- **gtest** (`src/Disks/tests/`):
  - `MountLeaseKeeper` renewal with a mock backend where the mount key is deleted after claim →
    `renewOnce`/`onRenewMismatch` throws `FILE_DOESNT_EXIST`, **not** `LOGICAL_ERROR`, and bumps
    `CasMountLeaseLost` (RED before the fix: `LOGICAL_ERROR`). Mirror the existing mount-classification
    tests in `gtest_cas_mount.cpp`.
  - `ContentAddressedMetadataStorage` shutdown→access cycle: after `shutdown()`, a store operation
    auto-remounts (re-runs `startup()`), against a local-emulated backend.
- **stateless/integration:**
  - `04295_content_addressed_mutation_no_leftovers.sh` as the end-to-end guard: no abort on debug/ASan,
    clean teardown via the new commands.
  - A focused stateless test exercising `UNMOUNT` → reuse (auto-remount) → `FSCK` returns a clean
    report on a CAS disk.

## Out of scope {#out-of-scope}

- Automatic CAS-disk teardown on `DROP TABLE`/`DROP DATABASE` (the `Context` disk-selector leak). The
  leak persists; `UNMOUNT` is the operator/test-driven clean path, and Part 1 makes the residual
  `rm -rf`-under-live-mount non-fatal.
- The GC scheduler's `CORRUPTED_DATA` "gc/state vanished" retry-spam after a bare `rm -rf` without an
  `UNMOUNT` (same underlying leak; non-fatal today). Tests that use `UNMOUNT` avoid it entirely.
- Broadening the abort-hardening to the genuine `superseded` / `foreign_writer` cases (kept fatal by
  the user's decision — real single-writer violations).

## Acceptance {#acceptance}

- Part 1 gtests green; `04295` no longer aborts on a debug/ASan run; `CasMountLeaseLost` visible in
  `system.events` after a mount loss.
- `SYSTEM CONTENT ADDRESSED UNMOUNT <disk>` stops threads and the disk auto-remounts on next access
  (gtest + stateless).
- `SYSTEM CONTENT ADDRESSED FSCK <disk>` returns a leftover report online.
- The "no-leftovers" tests pass with the new GC RUN → FSCK → UNMOUNT → rm -rf teardown.
- Full CA gtest gate green.
