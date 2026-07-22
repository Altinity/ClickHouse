# CAS disk: self-quiesce on vanished backing, and drop the runtime mount/unmount reuse lifecycle

Status: supersedes the disk-lifecycle parts of `2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md`
(rev.2) and discards `2026-07-22-cas-disk-lifecycle-automount-design.md` (an eject/auto-mount exploration
that produced no code). It keeps that first spec's Part 1 (abort-hardening) and Part 6 (benign-absent
probes) and rolls back its Parts 2–4 (the `Dormant` state and the `SYSTEM CONTENT ADDRESSED MOUNT/UNMOUNT`
*reuse* lifecycle).

## Why this exists — the conclusion of a long design loop

The 2026-07-21 work fixed a real CI abort (a background CAS thread aborting when a test `rm -rf`'d the pool
dir under a live mount) and, to give tests a clean stop/reuse story, added an explicit disk-lifecycle
state machine: `UNMOUNT` drains + quiesces a disk into a persistent `Dormant` state, `MOUNT` re-mounts it
for same-name reuse, `FSCK` runs dormant-only.

Operating and auditing that revealed the `Dormant` state is the root of a whole fragility class:

- A persistent, reachable-but-not-live disk in the registry forces every generic all-disks sweep
  (`DROP TABLE` finalizer, orphaned-parts search, `Freeze`, `system.*`) and every table load into a bad
  choice: throw (hang — the original bug) or answer benign-absent (silent-skip — a new class of
  permanent ref leaks and empty-loads the audit found: DROP reclaim skipped, `<readonly>`-replica ATTACH
  silently empty, etc.).
- Making the not-live disk *disappear* (evict it from the two disk registries) is safe (audited) but only
  moves the problem: you cannot cleanly know when a disk shared via untracked `shared_ptr` is unused, you
  cannot stop its mount-lease/heartbeat while anything still uses the pool (contract violation), and you
  cannot wait for a week-long backup to drain. Every variant reduces to "a not-live-but-held disk that
  throws" — `Dormant` by another name.

The realization: **we never actually needed runtime remount/reuse of the *same* shared disk.** That
ambition is what created the entire `Dormant`/eject/auto-mount problem. Drop it, and the problem
dissolves. The genuine needs are met minimally without any persistent not-live-reusable state.

## The genuine needs, and how each is met

| Need | Solution |
|---|---|
| The original abort — a background thread must not abort the server when the backing vanished | **Part 1** (already landed) + **self-quiesce** (below) |
| Stop the background retry-spam after a bare `rm -rf` of the pool dir | **Self-quiesce on vanished backing** |
| `05020_content_addressed_fsck` (and the CA stateless family) failing on retry | **Unique per-invocation disk names/paths in the tests** — no reuse of a stopped disk, so no `Dormant`/inert reuse |
| `FSCK` | **On a mounted disk** + `meta_without_body` hardening (dormant-only was a false guarantee vs another node's GC) |

## Design

### 1. Kept as-is: Part 1 abort-hardening, Part 6 benign-absent probes

- **Part 1** (`CasServerRoot.cpp`, `ProfileEvents.cpp`): a vanished/absent mount-lease at renewal and at
  terminate is `FILE_DOESNT_EXIST` / a no-op release, never `LOGICAL_ERROR` (which aborts debug/ASan at
  construction). `CasMountLeaseLost` counter. Unchanged.
- **Part 6** (`ContentAddressedMetadataStorage`): a not-`Mounted` disk answers the read-only
  existence/enumeration surface as absent/empty (`existsFile`/`existsDirectory`/`existsFileOrDirectory`
  → false, `isDirectoryEmpty` → true, `listDirectory`/`iterateDirectory` → empty,
  `getStorageObjectsIfExist` → nullopt); content/size/mutation still throw. Kept — and now **safe**,
  because (see §5) the only not-`Mounted` state in this design is *inert-broken* (backing genuinely
  gone), so a sweep's benign-skip of it can never silently skip real reclaim: there is nothing to
  reclaim. The `Dormant`-with-intact-data state that made Part 6's benign-skip a permanent-leak hazard
  (audit findings H1 / #1a / #2) **no longer exists**.
- Also kept (independently good, already landed): the single coherent `poolAccess()` snapshot (fixes the
  straddle bug), atomic-publish `startup()`, and the `ca-fsck` offline-applet rename.

### 2. New: self-quiesce on vanished backing {#self-quiesce}

When a mounted CAS disk detects that its **backing store has vanished** (the pool dir / mount key /
pool-meta is gone — an operator or test `rm -rf`'d it), it cleanly **stops ALL of its own background
work** and becomes inert, with one clear WARNING log line. Concretely:

- Detection reuses Part 1's existing "vanished" classification: the mount-lease renewal already detects
  the mount key absent (`onRenewMismatch`'s `!got` branch) and stops renewing fail-closed. This is the
  trigger.
- On that trigger the disk's `on_lost` path is extended from "latch the write fence to lost" to also:
  **stop the GC scheduler** (`CasGcScheduler::stop()` — joins its threads; this is what removes the
  retry-spam), stop any remaining background workers, transition the disk to an **`Inert`** state, and
  log:

  > `content-addressed disk 'X': backing store vanished (rm -rf'd?) — stopped all background work; disk is now inert. Recreate the disk (new name) or restart the server to use it again.`

- This is the honest end state of the legacy misuse the 2026-07-21 spec explicitly left as out-of-scope
  ("the GC scheduler's retry-spam after a bare `rm -rf` with NO `UNMOUNT`"): Part 1 made it non-fatal;
  self-quiesce makes it *quiet and terminal* instead of a spinning zombie.
- Scope: this is only for the **vanished** case (backing physically gone). The other lease-loss classes
  (superseded / foreign-writer / gc-fenced — another node took over, backing intact) keep their existing
  Part 1 behavior (fence latched, renewal stopped); they are not "backing gone" and do not make the disk
  inert.
- Optional (decide in the plan, not required): a proactive one-way `SYSTEM CONTENT ADDRESSED STOP <disk>`
  verb that triggers the same self-quiesce on demand, so a test can stop the background work *before* it
  `rm -rf`s and never see even a brief spam window. This is a one-way "stop" button, NOT a
  mount/unmount reuse pair — there is no re-mount of the stopped disk.

### 3. Inert-disk semantics — what "use it again" does {#inert}

The `Inert` state is a **terminal failure** (the data is genuinely gone), not a reusable pause:

- **Existing loaded table's queries** (the table under which the `rm -rf` happened): real ops (`SELECT`
  reading parts, `INSERT`) fail **loudly** with a clear message —
  *"content-addressed disk 'X': backing store vanished — disk is inert; recreate the disk (new name) or
  restart to use it again."* Correct: you deleted its data, the table is legitimately broken. No silent
  zero-row read.
- **All-disks sweeps**: benign-absent (Part 6) → skip. No hang; the skipped reclaim is harmless (nothing
  to reclaim — the backing is gone).
- **A new `CREATE`/`ATTACH` on the same disk name**: `getOrCreateDisk` returns the same inert object
  (no eviction in this design), so it fails the same way — the name is dead until server restart. In the
  tests this never happens (unique names, §4); for a config disk, `rm -rf`-ing its backing is a severe
  operator error whose natural recovery is a restart (which recreates the disk from config — a fresh,
  empty pool if the dir is empty).
- **No auto-recovery-by-reuse.** Re-initializing an inert disk transparently on next use *is* the
  lazy-auto-mount whose blockers (lock-order deadlock, sweep-vs-real ambiguity, ATTACH benign-absent
  data-loss) this whole design is retreating from. We do not do it. Recovery is explicit: a new name
  (tests) or a restart (config disks). A broken disk behaves like a broken disk.

### 4. Tests: unique per-invocation disk names/paths

The CA stateless tests that reuse a fixed disk name/path and manage its lifecycle switch to
**unique-per-invocation** names/paths (`${CLICKHOUSE_TEST_UNIQUE_NAME}` / `${CLICKHOUSE_USER_FILES_UNIQUE}`,
as `04290`/`04295` already do). This is the real fix for `05020_content_addressed_fsck` and the family:
with a unique disk per test invocation, a retry never reuses a stopped/inert disk, so the failure mode
(`GC RUN` on a reused not-mounted disk) cannot occur. Teardown is simply: `DROP TABLE` → (optional
`STOP`) → `rm -rf ${POOL_DIR}` — safe because Part 1 + self-quiesce mean no background thread is left
spinning on the removed dir.

### 5. `FSCK` on a mounted disk, `meta_without_body` hardened

`SYSTEM CONTENT ADDRESSED FSCK <disk>` (and the offline `ca-fsck`) run on the **mounted** disk; the
dormant-only requirement is dropped (there is no `Dormant` state anymore, and dormant-only was in any
case a false guarantee — it stops only THIS node's GC while another node sharing the pool keeps deleting
`body`-then-`meta`). Correctness under any node's concurrent GC comes from hardening the one racy check:
`meta_without_body` is a hard ERROR **only if the blob is still referenced** (a `blobStillReferenced`
re-check via a fresh `recoverRefTable`, the same pattern the dangling class already uses); an
unreferenced meta-without-body is benign GC churn (a soft/label class, not an ERROR). The one-row summary
output shape is unchanged.

## Rollback of the 2026-07-21 landed lifecycle

The following, already merged on `cas-gc-rebuild`, is reverted or simplified (detailed task-by-task list
belongs in the plan):

- The persistent **`Dormant`** state and its `MountState` machine collapse to `Mounted` / `Inert`
  (inert = self-quiesced-broken; there is no reusable not-mounted state). The `Unmounting` transient and
  the drain-to-sole-owner logic go away with the reuse lifecycle.
- `SYSTEM CONTENT ADDRESSED UNMOUNT` / `MOUNT` as a **reuse lifecycle** (drain → `Dormant` → re-`MOUNT`)
  is removed. (Optionally a one-way `STOP` verb remains — §2.)
- `SYSTEM CONTENT ADDRESSED FSCK` moves from dormant-only to on-mounted (§5).
- The `04290`/`04295` "no-leftovers" teardown pattern (`GC RUN → UNMOUNT → FSCK → rm -rf`) simplifies to
  unique-names + `rm -rf` (§4); `GC RUN`'s `pending_*` drain columns stay (harmless, already landed).
- The 2026-07-22 eject / generic `SYSTEM UNMOUNT DISK` / auto-mount exploration is discarded (no code
  landed; specs marked superseded).

Kept from 2026-07-21: Part 1, Part 6, `poolAccess` snapshot, atomic startup, `ca-fsck` rename, the new
AccessTypes/verbs that remain relevant.

## Error handling

- Vanished backing → self-quiesce: WARNING log, background work stopped, disk `Inert`. No abort, no
  retry-spam.
- Use of an `Inert` disk: probes benign-absent; real ops throw the clear "backing vanished — inert;
  recreate/restart" message (a dedicated code/message, not a generic "not mounted").
- `FSCK` on a non-CA disk → `BAD_ARGUMENTS`; unknown disk → `UNKNOWN_DISK`.

## Testing

- **gtest:** self-quiesce — a mounted disk whose backing is cleared (test backend map wiped) stops its GC
  scheduler + lease keeper and goes `Inert`, emitting the WARNING once (RED before: threads keep
  spinning); an `Inert` disk answers probes benign-absent and throws the clear vanished-message on a real
  op; `meta_without_body` re-check downgrades an unreferenced meta-without-body to benign and keeps a
  referenced one a hard ERROR.
- **stateless:** the CA family on unique disk names — `05020` and the no-leftovers family pass on retry
  (the original failure) under a sanitizer build; `FSCK` on a mounted disk returns the clean summary; a
  bare `rm -rf` of the pool dir after `DROP TABLE` does not abort or spam (self-quiesce fires) — the
  debug/ASan lanes stay up.
- Full CA gtest gate (definitive filter) green; the `05020`/family sanitizer lanes green.

## What we give up (and why it is fine)

No runtime remount/reuse of the *same* disk: to reuse, use a new name (tests) or restart (config disks).
That capability was the sole source of the `Dormant`/eject/auto-mount fragility cascade, and nothing real
required it. In exchange the entire hazard class (H1/H2/#1a/#2, the guard-coverage worry, the
lease-contract-vs-drain impossibility) disappears, with zero generic/upstream code touched and a broken
disk that honestly behaves as broken.
