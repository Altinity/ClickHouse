# CAS mount-lease abort hardening + explicit disk lifecycle (MOUNT / UNMOUNT / FSCK) design

Status: user-approved direction (2026-07-21), rev.2. Rev.1 was adversarially reviewed (codex
gpt-5.6-sol, high) and its Parts 2–4 were found under-designed (six blockers); this revision
replaces the lazy auto-remount / online-FSCK design with an explicit, idempotent lifecycle state
machine that resolves every blocker. The review's findings are recorded inline where they shaped
the design.

## Context {#context}

CI PR #2073 (`Stateless tests (amd_debug, sequential)`) crashed with a `LOGICAL_ERROR`
(`CAS mount-lease: key '…' was touched by a foreign writer — failing closed, never re-minting`,
STID 3982-3b48) that aborted the server and failed unrelated tests as collateral. Root cause chain,
confirmed from the server log + code:

1. A custom CAS disk created via the inline `disk(...)` AST function is cached forever in the
   `Context` disk selector (`Context::getOrCreateDisk`, no teardown). Its `CasPool` background
   threads (`ContentAddressedGC` scheduler + `MountLeaseKeeper` renewal) keep running after every
   table using the disk is dropped — `Pool::~Pool` (the clean-farewell teardown) never runs because
   the disk object is never destroyed.
2. The `04295_content_addressed_mutation_no_leftovers.sh` "no-leftovers" test drops the table, polls
   the pool dir until GC empties it, then `rm -rf`s the pool dir — deleting the backing store out
   from under the still-running threads.
3. The renewal thread's next `renewOnce` fails its `putOverwrite`; `MountLeaseKeeper::
   onRenewMismatch` re-reads the mount key, finds it **absent**, cannot classify, and falls through
   to the base `SingleWriterSlot::onRenewMismatch` → `throw LOGICAL_ERROR`. In debug/ASan builds the
   exception **constructor** aborts the process (`abort_on_logical_error`) — `backgroundLoop`'s
   `catch` never gets a chance. In release builds the catch swallows and logs; no crash. The crash
   is a debug/ASan artifact of classifying an environmental condition (backing store deleted) as a
   logic-invariant violation.

Two problems, both addressed here:

- **The abort (Part 1):** a background CAS thread must never abort the server because its backing
  store vanished — that is environmental, not a logic error.
- **The lifecycle (Parts 2–4):** CAS disks have no clean stop/verify/reuse story, so tests fall back
  to `rm -rf` under a live mount (broken GC retry spam, the abort above, and a later test finding
  the cached disk broken). The fix is an **explicit, idempotent** lifecycle: `SYSTEM CONTENT
  ADDRESSED UNMOUNT / MOUNT / FSCK`, wired into test setup/teardown.

Deliberately NOT in scope: automatic teardown on `DROP TABLE`/`DROP DATABASE` (would couple generic
`DROP`/`Context` code; with the state machine in place it becomes a trivial future hook), and any
form of lazy remount from query paths (rejected by review — see Part 2).

## Part 1 — abort-hardening + `CasMountLeaseLost` counter {#part1}

**Files:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`,
`src/Common/ProfileEvents.cpp`.

Review-verified facts this rests on: the abort is at `LOGICAL_ERROR` construction (only
`LOGICAL_ERROR` is special-cased; `FILE_DOESNT_EXIST` is on no abort list); `renewOnce` sets
`last_renew_failure_was_confirmed_mismatch = true` before every `onRenewMismatch` call, so a
non-`LOGICAL_ERROR` throw still stops the renewal loop, reaches `onRenewFailed`, and latches the
write fence to lost (fail-closed, never re-mint); `MountLeaseKeeper` is the only
`SingleWriterSlot` subclass and there are no other `onRenewMismatch` callers.

**1a. Renewal path.** In `MountLeaseKeeper::onRenewMismatch`, add an explicit branch before the
base-class fall-through:

```cpp
if (!got)
{
    /// The mount slot object VANISHED (backing store deleted under a live mount -- e.g. an operator
    /// or test rm -rf'd the pool dir). This is an ENVIRONMENTAL condition, not a logic error: there
    /// is no foreign writer to fail closed against. Stop renewing (fail-closed: the write fence
    /// latches to lost, we never re-mint) WITHOUT aborting the server -- LOGICAL_ERROR here aborts
    /// debug/ASan builds at exception construction.
    ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
    emitMountEvent(event_sink, CasEventType::MountConflict, srid, "vanished", nullptr,
        "mount slot object vanished (backing store deleted under a live mount) -- stopping renewal, fail-closed");
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
        "CAS mount-lease: key '{}' vanished (backing store deleted under a live mount) -- "
        "stopping renewal, fail-closed (never re-minting)", mismatched_key);
}
SingleWriterSlot::onRenewMismatch(mismatched_key);
```

**1b. Terminate path (review finding #7).** The clean-release path
(`MountLeaseKeeper`'s terminal write during `terminate`, `CasServerRoot.cpp:~859-894`) also throws
`LOGICAL_ERROR` when the lease body is absent after a failed terminal conditional write. The same
`rm -rf` scenario reaches it: renewal dies non-fatally (1a), then pool teardown
(server shutdown or the new `UNMOUNT`) runs clean-release, observes the lease absent, and would
abort at throw construction. Classify absent-lease during terminate the same way: environmental /
already-gone → emit the event, count `CasMountLeaseLost` (if not already counted for this loss),
and complete the terminate as a no-op release (the lease object we would delete is already gone —
the desired end state) or throw `FILE_DOESNT_EXIST` where a throw is required by the caller's
contract. Genuine present-body foreign/superseded cases stay fatal `LOGICAL_ERROR` in both paths.

**1c. Counter.** New ProfileEvent `CasMountLeaseLost` (operator-facing description in the current
house style: counts terminal mount-lease losses — vanished / superseded / foreign; non-zero means a
mount was lost, investigate via `system.content_addressed_log` `MountConflict` rows). Incremented on
the non-recoverable branches: `vanished` (new), `superseded`, `foreign_writer`; NOT on the
recoverable `fenced_by_gc` branch (normal GC-race noise). Review confirmed these points are
reachable and do not double-count with `onRenewFailed` (which only logs).

## Part 2 — explicit lifecycle state machine + `UNMOUNT` / `MOUNT` {#part2}

**Files:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
ContentAddressedMetadataStorage.{h,cpp}` (state machine), `src/Parsers/ASTSystemQuery.{h,cpp}`,
`src/Parsers/ParserSystemQuery.cpp`, `src/Access/Common/AccessType.h`,
`src/Interpreters/InterpreterSystemQuery.{h,cpp}` (verbs, modelled on the existing
`CONTENT_ADDRESSED_GC_RUN` wiring), `tests/queries/0_stateless/01271_show_privileges.reference`.

### Why not lazy auto-remount (review blockers #1, #3, #4, #5) {#why-explicit}

Rev.1 proposed `UNMOUNT` = `shutdown()` + lazy `ensureMounted()` in `store()`. Review showed:
`shutdown()` only resets shared pointers — the real teardown (lease retirement, write-lane drain,
thread joins, farewell) runs in `Pool::~Pool`, which fires only when the LAST outstanding `PoolPtr`
snapshot dies, so `UNMOUNT` was not a quiescence barrier and "safe `rm -rf`" was false (#1); lazy
`startup()` under `pointer_mutex` inverts the documented `gc_scheduler_mutex → pointer_mutex` order
(deadlock, #3); `startup()` publishes `cas_store` before the facade/probe/scheduler are built, so a
mid-startup throw leaves a half-initialized mount that later calls treat as complete (#4); and
several operations take `store()` + `partAccess()` separately, so a remount between them splits one
operation across two mount generations (#5). All four are eliminated by making the lifecycle
explicit and synchronous.

### The state machine {#state-machine}

`ContentAddressedMetadataStorage` gains `enum class MountState { Mounted, Unmounting, Dormant }`,
stored alongside `cas_store` under the existing `pointer_mutex`, plus a new `lifecycle_mutex` —
outermost, taken ONLY by the admin verbs (`MOUNT`/`UNMOUNT`/`FSCK`) and initial `startup()`; query
threads never take it, so no lock-order inversion is possible. Admin paths keep the documented
`gc_scheduler_mutex → pointer_mutex` order beneath it.

Initial mount is UNCHANGED: disk creation (server start for config disks, first `getOrCreateDisk`
reference for inline disks) calls `startup()` exactly as today → state `Mounted`. `Dormant` is
in-memory only, entered only via `UNMOUNT`, and never persists across a server restart (after
restart the disk is recreated and mounts normally; if the pool dir was removed, a fresh empty pool
is minted — review confirmed `Pool::open` handles an empty backing dir, minting fresh pool meta and
a fresh writer epoch).

**Operation gate:** every pool-touching entry point refuses when state != `Mounted` with a clear
retryable error (`CANNOT_OPEN_FILE`-family or a dedicated code): *"content-addressed disk '<name>'
is unmounted — run SYSTEM CONTENT ADDRESSED MOUNT '<disk>'"*. To close review finding #5's
bypasses, the gate lives in ONE coherent accessor returning a paired snapshot `{PoolPtr,
part-access facade}` taken under a single `pointer_mutex` acquisition; `tryGetInManifestBytes`,
`getBlobViewPlan`, and any other site currently taking `store()`/`partAccess()` separately are
routed through it.

### `SYSTEM CONTENT ADDRESSED UNMOUNT <disk>` {#unmount}

A synchronous quiescence barrier, idempotent and resumable:

1. **Live-table guard (review blocker #2).** Refuse (`BAD_ARGUMENTS`-family error listing the
   tables) if any loaded table's storage policy references this disk — traversal pattern as in
   `InterpreterSystemQuery::restartDisk`. The scan covers ALL disk names sharing this
   `ContentAddressedMetadataStorage` (a CAS cache wrapper shares one metadata storage between the
   base and cache disks — review finding #8), and the handler documents that unmounting any alias
   unmounts the shared pool.
2. State → `Unmounting` (under `pointer_mutex`): the operation gate now refuses NEW operations.
3. Stop the GC scheduler (existing `shutdown()` logic), release the part-access facade.
4. **Drain:** wait until the member `cas_store` is the only remaining owner
   (`use_count() == 1`; in-flight part writes pin the pool via `pin_owner`/`shared_from_this`, so
   the drain naturally waits for them). Bounded wait (default ~30 s, slice-polled). On timeout:
   throw `TIMEOUT_EXCEEDED` ("disk busy — N pool references still live"), state STAYS `Unmounting`
   (new ops still refused); a repeated `UNMOUNT` resumes from this step. Resumable-continuation was
   chosen over rollback: rollback would need to restart the GC scheduler and re-publish the facade,
   and the disk was being decommissioned anyway.
5. `cas_store.reset()` — with the drain complete this runs `Pool::~Pool` **synchronously inside the
   command**: lease clean-release (hardened by Part 1b), write-lane drain, background thread joins,
   farewell. State → `Dormant`.

After a successful `UNMOUNT` there are provably zero users of the old pool and zero live CAS
threads for this disk → a subsequent `rm -rf` of the pool dir is genuinely safe.

**Idempotency:** `UNMOUNT` on `Dormant` is a no-op success; on `Unmounting` it resumes; on
`Mounted` it performs the sequence. Safe to call unconditionally in teardown.

### `SYSTEM CONTENT ADDRESSED MOUNT <disk>` {#mount}

Explicit remount for same-process reuse (the ONLY scenario needing it — see initial-mount note
above). Under `lifecycle_mutex`: valid from `Dormant`; runs the (refactored) `startup()` and flips
state → `Mounted`.

**`startup()` atomic-publish refactor (review blocker #4):** build everything (backend, pool via
`Pool::open`, pool_uuid, capability probe, part-access facade, GC scheduler) into locals; publish
into members under `pointer_mutex` as the LAST step, together with state → `Mounted`. A throw at
any point publishes nothing — the disk stays `Dormant` and `MOUNT` is cleanly retryable. This also
fixes the pre-existing half-initialized-startup hazard for the initial mount.

**Idempotency:** `MOUNT` on `Mounted` is a no-op success (setup can call it unconditionally,
removing any need for a test to know whether a predecessor unmounted); on `Dormant` it mounts; on
`Unmounting` it throws "unmount in progress — retry UNMOUNT first" (deliberate asymmetry: MOUNT
never tries to revive a half-torn-down state; finish the unmount, then mount).

**Optional future improvement (not in this scope):** auto-`MOUNT` at disk-resolution time
(`getOrCreateDisk`/`DiskFromAST` returning a cached `Dormant` CAS disk invokes the same serialized
mount routine) — transparent same-name reuse for inline disks with no query-path laziness. Deferred:
idempotent `MOUNT` in setup covers the test need with zero generic-code changes.

## Part 3 — `SYSTEM CONTENT ADDRESSED FSCK <disk>` (dormant-only) {#part3}

**Files:** the same verb-wiring set as Part 2 + a handler in `InterpreterSystemQuery.cpp`.

Review blocker #6: `runFsck` is a multi-phase scan (ref recovery → blob LIST → manifest LIST) with
NO coherent snapshot — phases can be minutes apart, and its own comments admit it; the offline
applet refuses writable mounts for exactly this reason. Running it against a live mutating pool
yields false `unreachable`/dangling verdicts. Rev.1's "online FSCK" is therefore dropped.

**Design:** `FSCK` is valid only when the disk is `Dormant` (or configured observe-only/readonly).
From `Mounted` it fails with *"disk is mounted — run SYSTEM CONTENT ADDRESSED UNMOUNT first (FSCK
requires a quiesced pool)"*. The handler opens a TEMPORARY observe-only read-only pool view
in-process (the same open mode the offline `clickhouse-disks` applet uses), runs
`runFsck(Pool&, detail)`, returns the report as a result set (one row per object: class
`reachable`/`unreachable`/`orphaned-unaccounted`/`corrupt` + key + size; column shape mirrors the
offline applet's output so the two surfaces stay consistent — review confirmed the
`CONTENT_ADDRESSED_*` handlers can return a client-visible `BlockIO` result set), and closes the
temporary view before returning. No coherence caveats: the pool is quiesced by construction.

## Part 4 — GC drain predicate + test integration {#part4}

**Files:** `InterpreterSystemQuery.cpp` (GC RUN result columns),
`tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.sh`,
`04290_content_addressed_no_leftovers.*`, and the rest of the "no-leftovers" family.

Review finding #9: "GC RUN ×N until drained" was not implementable — `GC RUN` executes one round
and returns per-round counters with no authoritative "nothing pending" indicator, while
condemnation/graduation need multiple rounds.

**Design:** extend the `SYSTEM CONTENT ADDRESSED GC RUN` result set with the scheduler's pending
totals after the round: `pending_candidates`, `pending_condemned`, `pending_retired` (the GC state
knows its sets). The deterministic drain predicate is "one more round returns all three = 0". Tests
loop `GC RUN` (bounded retries, small sleep between rounds for grace/watermark timing) until the
predicate holds.

**Teardown pattern** for the no-leftovers family (replacing `poll dir empty → rm -rf`):

```
DROP TABLE ... SYNC
SYSTEM CONTENT ADDRESSED GC RUN <disk>      -- loop until pending_* all zero (bounded)
SYSTEM CONTENT ADDRESSED UNMOUNT <disk>     -- idempotent quiescence barrier
SYSTEM CONTENT ADDRESSED FSCK <disk>        -- dormant; assert 0 unreachable/orphaned/corrupt
rm -rf "${POOL_DIR}"                        -- now genuinely safe (no live threads/users)
```

Setup gains an unconditional idempotent `SYSTEM CONTENT ADDRESSED MOUNT <disk>` where the same disk
name may be reused within one server session.

## Part 5 — offline applet: `ca-fsck` with `fsck` as deprecated alias {#part5}

**Files:** `programs/disks/CommandFsck.cpp`, `programs/disks/DisksApp.cpp` (dispatch key), callers:
`tests/integration/test_content_addressed_drop_pool_member/test.py`,
`tests/integration/test_content_addressed_ref_snaplog/test.py`, `utils/ca-soak/soak/fsck.py`,
`docs/superpowers/cas/08-testing-and-soak.md` §1.2.

Rename the primary command to `ca-fsck` (consistent with `ca-gc-dryrun`/`ca-gc-rebuild`/
`ca-inspect`) and keep `fsck` as a working deprecated alias (review finding #10: a hard rename
breaks the independently-keyed `DisksApp` dispatch and multiple tracked callers, plus unknown
external scripts). Update all tracked callers and docs to `ca-fsck`; the alias prints a one-line
deprecation note to stderr.

## Error codes, access, and privileges {#access}

- Unknown disk name → whatever `Context::getDisk` throws today (`UNKNOWN_DISK`), not
  `BAD_ARGUMENTS` (review finding #11).
- New `AccessType`s `SYSTEM_CONTENT_ADDRESSED_UNMOUNT`, `SYSTEM_CONTENT_ADDRESSED_MOUNT`,
  `SYSTEM_CONTENT_ADDRESSED_FSCK`, consistent with the existing family; update
  `01271_show_privileges.reference`; add grant/refusal coverage analogous to the existing GC-verb
  access test.

## Testing {#testing}

- **gtest:**
  - Part 1a: mount key deleted after claim → `renewOnce` throws `FILE_DOESNT_EXIST` (not
    `LOGICAL_ERROR`), `CasMountLeaseLost` bumped (RED before fix).
  - Part 1b: lease absent at terminate → clean no-op release / non-`LOGICAL_ERROR`, no abort under
    debug (RED before fix: constructs `LOGICAL_ERROR`).
  - Part 2: mount→unmount→mount cycle on a local-emulated backend: unmount drains (a held `PoolPtr`
    blocks it until released; timeout path throws and resumes), state gates refuse ops when not
    `Mounted`, remount after `rm -rf`-equivalent (cleared backing map) mints a fresh pool;
    idempotency of both verbs per the state table.
  - startup atomic-publish: injected failure mid-startup leaves the storage `Dormant` and
    retryable, with nothing published.
- **stateless:**
  - `04295`/`04290` no-leftovers family on the new teardown pattern — the e2e guard for the
    original crash (debug/ASan lane must stay up).
  - A focused test driving the full `MOUNT`/`UNMOUNT`/`FSCK` state table via SQL, including
    FSCK-refused-while-mounted and unmount-refused-with-live-table.
  - Privilege test for the three new AccessTypes.
- **Full CA gtest gate** green throughout.

## Out of scope {#out-of-scope}

- Automatic teardown on `DROP TABLE`/`DROP DATABASE` (future trivial hook on this state machine).
- Auto-`MOUNT` at disk resolution (recorded as optional future improvement in Part 2).
- Online (mounted-state) FSCK — requires a snapshot/revalidation protocol; dormant-only is the
  honest contract.
- The GC scheduler's `CORRUPTED_DATA` retry-spam after a bare `rm -rf` with NO `UNMOUNT` (the
  legacy misuse); Part 1 makes it non-fatal, `UNMOUNT` makes it avoidable.

## Acceptance {#acceptance}

- Part 1 gtests green; a debug/ASan run of `04295` (old-style `rm -rf`, pre-Part-4 pattern) no
  longer aborts the server; `CasMountLeaseLost` visible in `system.events`.
- `UNMOUNT` provably quiesces (gtest drain semantics) and is idempotent/resumable; `MOUNT` is
  idempotent with atomic publish; the no-leftovers family passes on the new teardown pattern with
  FSCK asserting zero leftovers.
- `ca-fsck` works; `fsck` alias works with a deprecation note; all tracked callers updated.
- Full CA gtest gate green.
