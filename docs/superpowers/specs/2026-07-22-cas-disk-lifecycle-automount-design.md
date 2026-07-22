# CAS disk lifecycle: self-healing auto-mount, full-quiesce UNMOUNT, mount-without-table, GC control, live FSCK

## Motivation

The explicit mount-lifecycle landed on 2026-07-21 (`SYSTEM CONTENT ADDRESSED MOUNT/UNMOUNT/FSCK`, spec
`2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md`). It resolved the debug/ASan abort and
gave operators a way to quiesce a pool instead of `rm -rf`-ing a live mount. But operating it revealed
gaps:

- **A Dormant disk is a landmine.** A custom `disk(...)` is cached forever in the `Context` disk
  selector (the disk-lifecycle-leak). After `UNMOUNT` it stays `Dormant` in that cache with no path
  back to `Mounted` except an explicit `SYSTEM ... MOUNT` of a disk that already exists. `05020_content_addressed_fsck`
  fails 100% on the sanitizer/debug stateless lanes because it uses a fixed disk path, ends `Dormant`,
  and a retry in the same long-lived server reuses the cached `Dormant` disk — the next `GC RUN` trips
  `throwNotMounted`. (Unmasked when the `ReadBufferFromMemory` UBSan fix stopped the server dying first.)
- **`UNMOUNT` does not *unconditionally* stop all background activity.** A concurrent manual GC round
  during the `Unmounting` window can resurrect a scheduler that re-pins the pool, so the drain times
  out and the mount-lease renewal thread stays alive (recoverable on retry, but a real hole in "quiesce
  on the first try").
- **`MOUNT` cannot mount a disk that has no table** (it takes a name that must already be registered),
  so there is no way to, e.g., bring up a pool purely to `FSCK` it.
- **`FSCK` is dormant-only** — and `Dormant` is a *false* guarantee: it stops only THIS node's GC, while
  another node sharing the pool keeps deleting `body`-then-`meta`, so the `meta_without_body` hard-ERROR
  check false-positives regardless of local dormancy.
- **No way to see mount state, no `UNMOUNT ALL`, no GC pause.** `system.content_addressed_mounts.state`
  is the mount-*lease* state, not the `MountState`; a `Dormant` disk does not even appear.

## Goals (from the 2026-07-22 brainstorm)

1. `UNMOUNT` stops **all** background activity of the disk — unconditionally, on the first try.
2. Remove **all side-effects** of a previously-used CA disk at test start/end. We do **not** eject the
   disk from the registry (that is a two-cache generic-upstream change — see Non-goals); a fully
   quiesced, inert `Dormant` husk that re-mounts on next use is enough.
3. `MOUNT` can bring up a disk — by name **or by full inline `disk(...)` description** — **without a
   table** (e.g. to run `FSCK`). This is an *addition* to auto-mount, not a replacement.
4. `FSCK` runs on a **mounted** disk (drop dormant-only), made correct under concurrent GC from **any**
   node by hardening the racy check, not by a local quiesce that cannot protect it.
5. Separate `GC STOP` / `GC START` handles.
6. Fixing the stateless CA tests (05020 and the family) falls out of the above — no per-test rewrites.

## Non-goals / constraints

- **Minimize the touch of upstream / generic code.** No disk eviction from
  `DiskSelector`/`StoragePolicySelector` (that needs new generic `erase` surface across two independent
  caches + an audit of every `getDisksMap()`/`getPoliciesMap()` snapshot holder). The design lives in
  `ContentAddressed*` + the already-sanctioned `SYSTEM`-verb family pattern (parser/AST/AccessType/
  interpreter). The **one** deliberate new generic-code edit is the table-bind hook (§2): a single
  CA-gated `ensureMountedIfContentAddressed(disk)` call in `MergeTreeData`'s existing table-construction
  disk loop. It is unavoidable there and only there — a CA-internal hook cannot distinguish a table
  binding a disk from an all-disk sweep probing it (both go through the same benign methods), so the
  distinction must come from the caller. It is one delegating line, a no-op for non-CA disks.
- No cluster-wide pool lock is introduced. `ON CLUSTER` remains the way to act pool-wide.

## Design

### 1. `MountState` and the access model

`MountState { Mounted, Unmounting, Dormant }` already exists (Task 4). The change is what an *access*
to a not-`Mounted` disk does, split by surface:

- **Existence / enumeration probes stay benign-absent (unchanged from Part 6):** `existsFile`,
  `existsDirectory`, `existsFileOrDirectory`, `isDirectoryEmpty`, `listDirectory`, `iterateDirectory`,
  `getStorageObjectsIfExist` return "absent/empty" on a not-`Mounted` disk and **never auto-mount**.
  This is what keeps a server-wide sweep (`DROP TABLE` finalizer, `system.remote_data_paths`, …) from
  spuriously re-mounting every `Dormant` CA disk.
- **Table-bind mounts (new): an explicit hook where the table binds its disks.** The one place that
  knows "a table is about to use this disk" (as opposed to "a sweep is probing all disks") is the
  *caller* — `MergeTreeData` at table construction. A CA-internal hook cannot make this distinction: the
  same benign method (`getStorageObjectsIfExist`, `iterateDirectory`) serves both an all-disk sweep
  (must stay absent) and an ATTACH's `format_version`/parts read (must see real state), and the method
  cannot tell them apart. So the mount is triggered by the caller: `MergeTreeData`, before it reads any
  disk metadata (in `initializeDirectoriesAndFormatVersion`, whose disk loop already runs for both
  CREATE and ATTACH, strictly before the `format_version` read and `loadDataParts`), calls one
  CA-gated helper per disk — `ContentAddressedMetadataStorage::ensureMountedIfContentAddressed(disk)` —
  a no-op for non-CA disks, and for a `Dormant` CA disk it re-mounts it. This is the single sanctioned
  touch of generic code; it is honest ("the table readies its disks") and robust (independent of which
  metadata call happens first).
- **Everything else on a not-`Mounted` disk fails closed** (`INVALID_STATE`) as today — no per-op
  auto-mount. Safe and sufficient because the **live-table guard** keeps a *bound* disk `Mounted` (a
  disk with any live table can never be `Dormant`), so an ordinary read/write never lands on a `Dormant`
  disk; the only `Dormant`-reachable callers are (a) a table binding it — handled by the table-bind hook
  above, for CREATE *and* ATTACH — and (b) admin ops on a table-less disk, which the operator brings up
  with an explicit `MOUNT` (by name or inline `disk(...)` — §4).

**Why this is safe (the codex blockers that killed rev.1's lazy remount are gone):** the live-table
guard forbids `UNMOUNT` while any live table references the disk, so **a `Dormant` disk has no live
tables** — an existing-table read therefore never lands on a `Dormant` disk (it is always `Mounted`).
Auto-mount is only ever reached by a *new* `CREATE TABLE` (write) or an admin op on a disk with no live
tables, so there is no live table to serve stale/empty data to (blocker #2). And it is triggered via
the admin path, not `store()` under `pointer_mutex` (blockers #1/#3/#4/#5) — see next.

### 2. `ensureMounted()` — the auto-mount hook (deadlock-free)

Rev.1's fatal flaw was calling `ensureMounted()` from inside `store()` under `pointer_mutex`, inverting
`lifecycle_mutex → gc_scheduler_mutex → pointer_mutex`. Here `ensureMounted()` is a **separate, earlier
hook** each real-work method calls **before** `poolAccess()` — while holding no CA mutex:

```
void ensureMounted():
    // Fast path: no lock beyond a brief pointer_mutex read of mount_state.
    state = load mount_state
    if state == Mounted: return
    if state == Unmounting: throw INVALID_STATE("unmount in progress; retry")   // do NOT resurrect
    // state == Dormant: slow path
    std::lock_guard lifecycle(lifecycle_mutex)
    if mount_state == Dormant: mountExplicitly()   // Dormant -> Mounted, atomic publish (Task 3)
```

- Fast path (the common case, `Mounted`) is a single cheap `mount_state` read — no `lifecycle_mutex`.
- Slow path takes `lifecycle_mutex` (outermost) and re-mounts via the existing idempotent
  `mountExplicitly()`. Lock order is respected because `ensureMounted()` holds no inner lock when it
  takes `lifecycle_mutex`.
- `Unmounting` is refused, not mounted — this is exactly what closes the goal-1 race: a GC round (or any
  real-work op) issued during an in-flight `UNMOUNT` gets `INVALID_STATE`, never a resurrected pool.
- TOCTOU: a concurrent `UNMOUNT` may flip the disk back to `Dormant`/`Unmounting` between
  `ensureMounted()` and `poolAccess()`; `poolAccess()` then throws `INVALID_STATE` as today (rare;
  caller-visible, retryable). No new hazard.

The two GC-round entry points (`runGarbageCollectionRoundNow`, `runOneGcRoundForTest`) currently gate
scheduler (re)creation on `!cas_store` only; they route through `ensureMounted()` (or equivalently gate
on `mount_state == Mounted`) so they can never construct a scheduler during `Unmounting`. This is the
goal-1 quiescence fix.

**Where the hook sits — `MergeTreeData` table construction, delegating to a CA helper.** In
`MergeTreeData::initializeDirectoriesAndFormatVersion`, whose `for (const auto & disk : getDisks())`
loop already runs for both CREATE and ATTACH (`need_create_directories = true`, trace-verified as the
unconditional header default) strictly before the `format_version.txt` `readFileIfExists` and
`loadDataParts`, add one CA-gated call per disk BEFORE the reads:
`ContentAddressedMetadataStorage::ensureMountedIfContentAddressed(disk)` (`tryFromDisk(disk)` → if CA,
`ensureMounted()`; a no-op otherwise). `ensureMounted()` runs the fast-path/`lifecycle_mutex` logic
above holding no CA mutex, so there is no lock inversion (rev.1 deadlocked only because it hung the
mount inside `store()` under `pointer_mutex`). This is the sole edit to generic code — one honest line
that says "ready the table's disks," not a CA-internal guess keyed off which metadata call happens to
come first.

**Why the caller must do it, and why it is correct for CREATE and ATTACH:**

- **CREATE** on a `Dormant` husk: the hook mounts it, then the fresh table's `format_version` read is
  genuinely absent (new uuid) and the write proceeds on the mounted disk.
- **ATTACH** on a `Dormant` disk: the hook mounts it **before** the `format_version` read, so the read
  returns the REAL on-disk value (not the benign-absent `std::nullopt` that would otherwise force the
  write branch to clobber `format_version` with a schema-derived value), and `loadDataParts`'s parts
  `iterateDirectory` sees the REAL parts (not a benign-empty listing that would silently attach an empty
  table). This is a real fix, not just convenience: today ATTACH-on-`Dormant` is safe only by the
  fragile accident that the `format_version` write always throws `INVALID_STATE` before `loadDataParts`
  runs, and `loadDataParts` has no independent mount check — the table-bind hook makes the safety
  intentional.

The GC/FSCK admin entry points additionally call `ensureMounted()` at their top so a round issued
during an in-flight `UNMOUNT` gets `INVALID_STATE` rather than resurrecting a scheduler (the goal-1
race). For a table-less `Dormant` disk this is not a convenience auto-mount — the operator brings it up
with an explicit `MOUNT` first.

### 3. `UNMOUNT` — unconditional full quiesce → inert `Dormant` husk

`unmountSynchronously()` already stops the GC scheduler, drains `store()` refs to sole ownership, runs
`~Pool` (which joins the mount-lease renewal thread, the self-remount thread, and — via the pool
refcount the drain waits on — the two one-shot detached threads), and sets `Dormant`. The only gap is
§2's resurrection race, closed by routing the GC round entry points through the mount gate. After that,
`UNMOUNT` unconditionally leaves an **inert husk**: the `DiskPtr`/`ContentAddressedMetadataStorage`
object remains cached (no upstream eviction), but owns zero threads and touches the backend not at all
until the next real-work access auto-mounts it. Tests may still `rm -rf` the pool dir after `UNMOUNT` —
now genuinely safe.

### 4. `MOUNT` — idempotent, by name or inline `disk(...)`, without a table

- **By name** (today): `SYSTEM CONTENT ADDRESSED MOUNT '<disk>'` — idempotent `mountExplicitly()`.
- **By inline description (new):** `SYSTEM CONTENT ADDRESSED MOUNT disk(type=..., metadata_type=content_addressed, ...)`.
  Parser gains a branch trying `ParserFunction` + `isDiskFunction` before the name fallback, storing the
  raw `ASTPtr` in a new `ASTSystemQuery::disk_function` field. The interpreter, when `disk_function` is
  set, resolves it to a `DiskPtr` via `DiskFromAST::createCustomDisk(ast, getContext(), /*attach=*/false)`
  (table-agnostic — its only inputs are the AST + `Context`), then mounts exactly as the by-name path.
  Identical AST args resolve to the same registered disk (hash-derived name), so `MOUNT disk(...)` then
  `UNMOUNT disk(...)` / `FSCK disk(...)` with the same literal args target the same disk. Use case:
  mount a pool that has no table to run `FSCK` on it.
- `UNMOUNT` and `FSCK` accept the same `disk(...)` inline form via the same parser/interpreter change.

### 5. `FSCK` — on a mounted disk, dormant-only dropped, `meta_without_body` hardened

- **Drop dormant-only.** `FSCK` runs against the live mounted pool (`store()` after `ensureMounted()`),
  no temporary observe-only pool required.
- **Harden the one racy check.** `meta_without_body` is a hard ERROR today from a single-pass LIST
  pairing with no re-check, but GC deletes `body` first and `meta` later (async, unbounded lag) — from
  *any* node — so "meta present, body absent" is a legitimate transient. Fix: a `meta_without_body`
  finding is a hard ERROR **only if the blob is still referenced** (re-resolve via a fresh
  `recoverRefTable`, the same `blobStillReferenced` re-check the dangling class already uses). An
  unreferenced meta-without-body is benign GC churn (report it as a soft/label class alongside
  `PendingGc`/`AwaitingGc`/`Unaccounted`), not an ERROR. This makes the check correct under concurrent
  GC from any node — which dormant-only never achieved.
- `GC STOP` before `FSCK` remains available for a maximally quiet run, but is **not** required.

### 6. `GC STOP` / `GC START`

`SYSTEM CONTENT ADDRESSED GC STOP '<disk>'` / `GC START '<disk>'`. `CasGcScheduler::stop()` is
re-startable and `start()`/`stop()` on the **existing** instance preserve `gc_id` and the lease
observation window (recreating the scheduler would reset the steal protocol against its own prior
incarnation — must not). Same snapshot-then-act locking as `shutdown()`/`unmountSynchronously()` but the
member is **not** reset and the disk stays `Mounted`; `GC START` lazily constructs the scheduler if
absent (mirroring the existing GC-round entry points), else `start()`s the stopped instance. A `Mounted`
disk with a stopped/absent scheduler is already a coherent existing state (`gcHealth()` returns
`nullopt`, the system table shows NULL health). Fix the one gap: `stop()` clears `i_am_leader` (or
surface an explicit `running` bit) so introspection does not show a stale leader.

### 7. `UNMOUNT ALL`

`SYSTEM CONTENT ADDRESSED UNMOUNT` with no disk (or an explicit `ALL`) iterates every CA disk
(`getDisksMap()` + `tryFromDisk`) and `unmountSynchronously()`-es each that has no live table; disks
with live tables are **skipped and reported** (not a hard failure), so teardown does not need to know
which disks are mounted. No eviction — each becomes an inert husk.

### 8. Introspection of `MountState`

`system.content_addressed_mounts` enumerates every CA disk (`getDisksMap()` + `tryFromDisk`), including
`Dormant` ones, and gains a `mount_state` column (`Mounted`/`Unmounting`/`Dormant`). Lease/health columns
are read only when `Mounted` (NULL/absent otherwise) so the table never trips `throwNotMounted` on a
`Dormant` disk. A `gc_running` bit (from §6) distinguishes "GC stopped" from "leading".

### 9. Tests fall out for free

With auto-mount, a `Dormant` husk re-mounts on the next real-work access, so `05020`'s retry
(`CREATE TABLE` → auto-mount → `GC RUN`) just works — no per-test rewrite. The CA stateless family is
otherwise unchanged. New behavior gets its own coverage (below). `05020` may additionally be tidied to
the setup-`MOUNT`/teardown-`UNMOUNT` idiom for clarity, but it is not required for green.

## Relationship to the 2026-07-21 design (what this supersedes / carries)

This spec builds on `2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md` (rev.2). Reconciled
explicitly so the two do not contradict:

1. **Supersedes Part 3 ("FSCK dormant-only is the honest contract") and the Out-of-scope line "Online
   (mounted-state) FSCK".** Rev.2 dropped online FSCK on review blocker #6 (multi-phase scan, no
   coherent snapshot). New evidence changes the verdict: `Dormant` stops only THIS node's GC while
   another node sharing the pool keeps deleting `body`-then-`meta`, so the `meta_without_body`
   hard-ERROR false-positives *regardless* of local dormancy — dormant-only was a false guarantee. The
   real fix is the `blobStillReferenced` re-check in §5, which makes the check correct under any node's
   GC (the reachability/dangling classes already re-check and were the other half of blocker #6).
   FSCK therefore moves to mounted-state; dormant-only is removed.
2. **Picks up the deferred auto-mount (Part 2 "Optional future improvement"), but CA-internally.**
   Rev.2 deferred auto-`MOUNT` and sketched it "at disk-resolution time (`getOrCreateDisk`/
   `DiskFromAST`)" — which is generic/upstream code. This spec instead auto-mounts **on real-work
   access inside `ContentAddressedMetadataStorage`** (§2), which needs no generic-disk change — the
   deliberate choice to honor "minimize upstream."
3. **Carries Part 4 unchanged.** The `SYSTEM CONTENT ADDRESSED GC RUN` `pending_*` drain columns and
   the no-leftovers teardown pattern (`04290`/`04295`) stay as implemented. One consequence of §5:
   `FSCK` no longer requires the preceding `UNMOUNT` (it runs mounted), so the teardown's `UNMOUNT` is
   now only for quiescing before `rm -rf`, not a precondition of `FSCK` — the existing pattern still
   works either way.
4. **Introspection + `UNMOUNT ALL` dedup shared storage.** Per rev.2 review finding #8, a CAS
   cache-disk wrapper shares one `ContentAddressedMetadataStorage` between the base and cache disk
   names. `UNMOUNT ALL` and `system.content_addressed_mounts` dedup by the storage pointer
   (`tryFromDisk`), so a shared pool is unmounted once and shown once.
5. **FSCK output shape stays the implemented one-row summary** (Task 7), not rev.2 Part 3's
   per-object listing (that was reduced to a summary + a future `DETAIL` keyword by YAGNI). This spec
   changes only FSCK's exclusivity and the `meta_without_body` classification, not its columns.

Unchanged and still in force from rev.2: Part 1 abort-hardening; the state machine, `lifecycle_mutex`,
live-table guard, resumable `UNMOUNT`, atomic-publish `startup()`, the single paired-snapshot gate
(`poolAccess`), and Part 6 benign-absent probes. Still out of scope (unchanged): automatic teardown on
`DROP TABLE`/`DROP DATABASE` (would couple generic `DROP`); registry eviction of the disk (a two-cache
generic-upstream change — see Non-goals). Known accepted trade-off: the inert `Dormant` husk stays in
the disk cache (the pre-existing disk-lifecycle-leak is not fixed here — the user chose "remove
side-effects," not "eject").

## Error handling

- `ensureMounted()` on `Unmounting` → `INVALID_STATE` "unmount in progress; retry" (never resurrect).
- Benign probes on not-`Mounted` → absent/empty (unchanged).
- `MOUNT`/`UNMOUNT`/`FSCK`/`GC STOP`/`GC START` on a non-CA disk → `BAD_ARGUMENTS` "is not a
  content-addressed disk"; unknown disk → `UNKNOWN_DISK`; inline `disk(...)` that is not content-addressed
  → `BAD_ARGUMENTS`.
- `MOUNT` while `Unmounting` → `INVALID_STATE` (finish the unmount first) — unchanged.
- Auto-mount of a husk whose pool dir was removed mounts a fresh empty pool — correct for a new table;
  the live-table guard guarantees no live table depended on the removed data.

## Testing

- **gtest** (`CaLifecycle` family): auto-mount — a `Dormant` storage serves a real-work op after
  transparently re-mounting; a benign probe on `Dormant` does NOT mount; `ensureMounted` on `Unmounting`
  throws `INVALID_STATE` (the resurrection race is closed); `MOUNT`/`UNMOUNT` idempotent; GC STOP then
  START on a mounted disk keeps it `Mounted` and leaves `gc_id` stable; FSCK `meta_without_body`
  reference-re-check downgrades an unreferenced meta-without-body to benign and keeps a referenced one a
  hard ERROR.
- **stateless**: extend the mount/unmount + fsck tests — `MOUNT disk(...)` without a table then `FSCK`
  it; `GC STOP`/`START`; `UNMOUNT ALL` skip-and-report; `system.content_addressed_mounts` shows
  `mount_state` incl. a `Dormant` row. Access-control rows for any new AccessTypes.
- **the original bug**: the 05020 retry-in-one-server scenario passes under a sanitizer build (the
  local `build_asan` reproduction harness) — auto-mount makes the retry `GC RUN` succeed.
- Full CA gtest gate (definitive filter) + the CA stateless family green.

## Rollout / definition of done

- All of the above land as `ContentAddressed*` changes plus the sanctioned `SYSTEM`-verb family
  extensions (parser/AST/AccessType/interpreter), plus exactly ONE generic edit: the CA-gated
  table-bind hook line in `MergeTreeData::initializeDirectoriesAndFormatVersion` (§2). No
  `DiskSelector`/`StoragePolicySelector`/generic-disk-registry edits.
- CI: `Unit tests (asan_ubsan/tsan)` stay green; the `amd_asan_ubsan/amd_debug/amd_tsan` stateless lanes
  clear `05020` and the CA family.
