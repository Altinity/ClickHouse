# CAS disk: lease-loss = throw, keep-threads-running, explicit STOP/START verbs

Status: **DESIGN — for review.** Supersedes the whole runtime-reuse lifecycle line.

Supersedes:
- `2026-07-22-cas-disk-self-quiesce-simplification-design.md` (Path B / self-quiesce+Inert — blocked by codex; its
  central "mount-key-absent ⇒ data-gone ⇒ benign-absent safe" assumption is false).
- `2026-07-22-cas-disk-lifecycle-automount-design.md` (eject/auto-mount — no code landed).
- Parts 2–4 of `2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md` (the landed Dormant/UNMOUNT/MOUNT
  reuse lifecycle, Task 4–8). **Part 1 (abort-hardening) is KEPT.**

Companion: `2026-07-22-cas-disk-lifecycle-problem-and-constraints.md` (the problem statement + constraints C1–C15
+ requirements R1–R10 this design must satisfy). Read it first.

---

## 1. The decision, in one paragraph

Drop the entire runtime mount/unmount/reuse lifecycle and its `Dormant` state. A CA disk is either **Running**
(background threads alive) or **Stopped** (explicit admin STOP). Access validity is gated dynamically on the
**current mount-lease validity**: lease valid → serve; lease invalid (transient outage, vanished backing, fenced
by another node) → **throw** on every access, including probes — *honestly and simply*, with **no benign-absent
lying**. On involuntary lease loss the background threads **keep running** (the lease keeper keeps retrying
renewal, GC keeps ticking but acts only as leader): if the backing returns, the lease is re-acquired and the disk
transparently becomes usable again — **auto-recovery, no operator action**. Deliberate quiescence is a separate,
explicit thing: `SYSTEM CONTENT ADDRESSED STOP '<disk>'` stops all background work and releases the lease;
`START` resumes. Because "throw on access" reintroduces the Task-8 server-wide `DROP` wedge, we add **one** small,
honest generic-code guard so a not-live CA disk that a dropped table never used cannot wedge that table's `DROP`.

This resolves the codex blockers that sank Path B: there is **no persistent not-live-with-intact-data state that
answers probes benign-absent** (blocker #2 / the silent-skip reclaim leak), and a **transient outage never
permanently disables a healthy disk** (blocker #1) — it throws only while the lease is down and auto-recovers.

## 2. State model

Two states. The not-live *condition* is dynamic, not a third persisted state.

```
                 lease valid
   ┌───────────────────────────────────────┐
   │                                        ▼
[Running] ── lease invalid (transient/vanished/fenced) ──► access THROWS
   │   ▲          (threads keep running; keeper retries; GC no-op-unless-leader)
   │   │                    lease re-acquired
   │   └────────────────────────────────────┘   ← AUTO-RECOVERY
   │
   │  SYSTEM CONTENT ADDRESSED STOP '<disk>'
   ▼
[Stopped]  (all threads joined, lease released, access THROWS)
   │
   │  SYSTEM CONTENT ADDRESSED START '<disk>'
   ▼
[Running]  (re-run startup, atomic-publish)
```

- **Running / lease valid** — normal operation.
- **Running / lease invalid** — every `poolAccess` throws `INVALID_STATE` (code 668) with a clear message
  ("mount lease not currently held — backing may be temporarily unreachable; retrying"). Threads stay alive.
  Auto-recovers when renewal next succeeds. This is *the* not-live condition for involuntary causes; it is **not**
  a persisted `Dormant`/`Inert`.
- **Stopped** — set only by the STOP verb. All background threads (GC scheduler + lease keeper) are stopped, the
  lease is released, the pool is torn down to the point where it holds no lease. `poolAccess` throws
  ("disk is STOPped — use SYSTEM CONTENT ADDRESSED START to resume"). Only START resumes.

Crucially: in **every** not-live case (Running/lease-invalid or Stopped), the probe methods
(`existsFile`/`existsDirectory`/`existsFileOrDirectory`/`isDirectoryEmpty`/`listDirectory`/`iterateDirectory`/
`getStorageObjectsIfExist`) **throw**, exactly like the content/size/mutation methods. There is no benign-absent
branch anywhere. (Part 6 is rolled back — see §4.)

## 3. Lease validity gating (keep Part 1; make throw the sole behavior)

`ContentAddressedMetadataStorage::poolAccess()` (`ContentAddressedMetadataStorage.cpp:~1548`, throwing via
`throwNotMounted()` at `:805`) already returns a coherent `{PoolPtr, part_access}` snapshot or throws. Change:
its gate becomes **"is the mount lease currently valid?"** instead of the `MountState==Mounted` check. The lease
keeper already tracks lease liveness (`system.content_addressed_mounts.state` = live/expired/terminated/fenced/
corrupt, sourced from `CasServerRoot` MountLeaseKeeper). `poolAccess` consults that liveness.

- **Part 1 stays**: the keeper's renewal path (`onRenewFailed` `CasServerRoot.cpp:810`, `onRenewMismatch !got`
  `:822/:864`, `backgroundLoop` `:1065`) must continue to treat a vanished/absent mount key as a recoverable
  lease-loss (`FILE_DOESNT_EXIST`/no-op, `CasMountLeaseLost` ProfileEvent), never `LOGICAL_ERROR`. This is what
  lets the thread survive to retry and auto-recover.
- **Remove** the `on_lost`-triggered self-quiesce/Inert transition that Path B proposed (never landed) — we do
  the opposite: keep the threads running.
- GC while lease invalid: the scheduler keeps ticking but takes no destructive action unless it holds GC
  leadership (`i_am_leader`), which it cannot without a live lease — so it is a safe no-op that resumes when the
  lease returns. GC must remain 404-tolerant (record + continue, never throw out of the fold — existing invariant).

## 4. Rollback of the landed Task 4–8

Remove, restoring the pre-lifecycle shape wherever these were added:

1. **`MountState` enum** (`Mounted`/`Unmounting`/`Dormant`) and every branch on it → collapse to the
   lease-validity gate of §3. `poolAccess`/`throwNotMounted` are **kept** but re-pointed at lease validity.
2. **`SYSTEM CONTENT ADDRESSED UNMOUNT` / `MOUNT`** verbs, their `AccessType`s, `ASTSystemQuery` fields, and
   `unmountSynchronously()` + the `Unmounting` drain loop. (Replaced by STOP/START — §5 — which are simpler:
   no reuse-of-same-object contract, no benign-absent.)
3. **Part 6 benign-absent probes** — the `isMounted()`-guarded absent/empty answers in the probe methods
   (`ContentAddressedMetadataStorage.cpp:~557-564`, `:1114` existsDirectory, `:1370` iterateDirectory-empty, and
   siblings). They now throw via the same lease gate. **This is the load-bearing rollback** — it is what makes
   the design honest, and it is what necessitates §6.
4. **FSCK-dormant-only** (Task 7) — FSCK runs on a Running disk (§7).
5. The **GC RUN → UNMOUNT → FSCK → rm -rf** teardown pattern in tests → replaced by §8.

Keep, untouched: Part 1, the `poolAccess` coherent snapshot, atomic-publish `startup()`, the `ca-fsck` rename,
the GC-round entry-point gating fix, `system.content_addressed_mounts` (extend to show Stopped disks — §5).

## 5. New verbs

### `SYSTEM CONTENT ADDRESSED STOP '<disk>'` / `START '<disk>'`

Full quiesce / resume of a named, already-registered CA disk.

- **STOP**: runs on the admin (query) thread — **not** the keeper thread, so joining the keeper thread is
  lifetime-safe (satisfies C6, avoids the self-join Path B had). Under `lifecycle_mutex → gc_scheduler_mutex →
  pointer_mutex` (C15): stop the GC scheduler (`CasGcScheduler::stop()`), stop the lease keeper
  (`stopBackground` `CasServerRoot.cpp:1051`), release the lease, and clear the published pool pointer so
  `poolAccess` throws the "STOPped" error. Sets the `Stopped` state.
- **START**: re-runs the atomic-publish `startup()` — re-acquire the lease, restart GC + keeper, publish the
  pool. Idempotent if already Running.
- **In-flight work when STOP is issued**: STOP must not tear the pool out from under a live user (C3/C4/C5 —
  a backup can hold the pool for a long time). STOP **refuses with a clear error if the disk is currently in
  use** (loaded tables bound to it, or a registered in-flight operation), rather than waiting or force-tearing.
  Operator drops/detaches the users first, then STOPs. (No week-long wait; no using-without-lease.)

### `SYSTEM CONTENT ADDRESSED GC STOP '<disk>'` / `GC START '<disk>'`

Granular: stop/restart **only** the GC scheduler. The lease keeper stays alive, the disk stays fully usable
(reads/writes work). `CasGcScheduler::stop()` must be re-startable — same instance preserves `gc_id`, and
`stop()` clears `i_am_leader` (or exposes a running bit) so a later `GC START` re-enters leadership cleanly.

### Introspection

`system.content_addressed_mounts` enumerates Stopped disks too (state column shows `stopped`), so an operator can
see which disks are quiesced and why access throws.

## 6. The one generic-code guard: don't let a not-live CA disk wedge `DROP` (R4)

Removing Part 6 (§4.3) means `existsDirectory` on a not-live CA disk throws. The `DROP TABLE` finalizer
(`DatabaseCatalog::dropTableFinally`, `DatabaseCatalog.cpp:1657-1665`) sweeps **all** disks from `getDisksMap()`
and probes each with `existsDirectory`; a throw there propagates, the table is re-queued
(`dropTablesParallel` `:1589`) and retried forever, and because the drop queue is **server-wide**, one not-live
CA disk wedges `DROP` for **every** table — including tables that never used it (the Task-8 bug). Unique disk
names do **not** help: the not-live disk still lingers in `getDisksMap()`.

**Guard:** wrap the per-disk probe+remove in the finalizer loop in try/catch and decide by
**policy-membership** (the check already exists inside `is_disk_eligible_for_search` at `:1650`,
`storage->getStoragePolicy()->tryGetVolumeIndexByDiskName(disk->getName())`):

- Disk **is** in the dropped table's storage policy (the table actually used it) → **rethrow / re-queue**
  (retry later; reclaim must never be silently skipped — no leak). If it is Stopped, the operator STARTs it and
  the retry drains; if the backing is transiently down, the retry drains on recovery.
- Disk **not** in the table's policy (foreign disk, swept only for best-effort orphan-parts search) →
  **log and continue** (skip this disk this round). Orphans on a currently-unsearchable disk are caught on a
  later round or by that disk's own GC; skipping is safe and does not leak the dropped table's data.

Apply the same skip-foreign-with-log pattern to the background orphan-directory sweep
(`DatabaseCatalog.cpp:~1986`, the `getDisksMap()` + `iterateDirectory("store")` loop) so a not-live CA disk
does not abort the whole orphan-cleanup pass.

**Known limitation (surface, don't hide):** if `table.table` is null (table not loaded at finalize time), the
policy is unknown, so we cannot prove the not-live disk is foreign → we conservatively **re-queue** (retry),
which means an unloaded table's `DROP` can wedge behind a not-live foreign disk until it recovers/STARTs. This
preserves correctness (never silent-skip the table's own disk) at the cost of a visible, recoverable stall.
Logged each round. (Open question for review: is reading the unloaded table's metadata to learn its policy worth
it, or is the conservative retry acceptable?)

## 7. FSCK on a Running disk

Drop the dormant-only requirement. FSCK runs on a Running (mounted) disk; concurrent GC (this node or another)
is expected. The hard `meta_without_body` finding
(`CasFsck.cpp:~262/:290/:563`) false-positives under the body-delete-then-meta-delete-later ordering
(`CasGc.cpp:410`). Harden: a hard `meta_without_body` (and the manifest-missing recovery finding) is reported
only after re-validating against the **authoritative current ref** (fresh `recoverRefTable`, the
`blobStillReferenced` pattern) **and** a direct **HEAD of the exact body object** — distinguishing "body
genuinely lost while still referenced" (real corruption) from "GC transient: body deleted, meta pending" and
"LIST omitted the body under eventual consistency". One-row summary output unchanged.

## 8. Tests

- **Rewrite `05020_content_addressed_fsck.sh` and the `04290`/`04295` family**: unique per-invocation disk
  **names** (`${CLICKHOUSE_TEST_UNIQUE_NAME}`) and paths (`${CLICKHOUSE_USER_FILES_UNIQUE}`) — the registry keys
  on **name** (C8), so a unique path alone is insufficient; both must be unique. Update every SQL predicate that
  referenced the old fixed name.
- **Teardown order**: `DROP TABLE ... SYNC` (or `database_atomic_wait_for_drop_and_detach_synchronously=1`)
  **before** any `rm -rf` of the pool dir, so the finalizer reclaims on a **live** disk. Never `rm -rf` a
  globally-configured standard CA disk's data — only unique-name per-test disks may be `rm -rf`'d.
- **New: STOP/START** — after STOP, a query against the disk throws the "STOPped" error; after START, it
  succeeds. STOP while a table is loaded on the disk is refused with a clear error.
- **New: GC STOP/START** — GC STOP halts the scheduler (observable: no new GC rounds); reads/writes still work;
  GC START resumes.
- **New: DROP-wedge guard (R4)** — table A on a CA disk, table B on a normal disk; STOP the CA disk; `DROP TABLE
  B` must complete (not wedge); `DROP TABLE A` re-queues and completes after `START`.
- **Auto-recovery** (gtest / integration, hard to do in stateless): lease key removed then restored → access
  throws in the gap, succeeds after → no restart needed.

## 9. Error handling summary

| Situation | Behavior |
|---|---|
| Lease valid | Normal. |
| Lease invalid (transient/vanished/fenced), Running | Every access + probe throws (668); threads keep running; auto-recovers on renewal. |
| Backing vanished, keeper renewal | Part 1: `FILE_DOESNT_EXIST`/no-op, never `LOGICAL_ERROR`; keeps retrying. |
| GC while lease invalid | No destructive action (no leadership); 404-tolerant; resumes on recovery. |
| STOP issued, disk idle | Threads joined, lease released, `Stopped`; access throws until START. |
| STOP issued, disk in use | Refused with a clear error. |
| DROP finalizer hits not-live CA disk, table used it | Rethrow → re-queue → retry (no silent-skip). |
| DROP finalizer hits not-live CA disk, foreign | Log + continue (skip this round). |
| FSCK hard finding | Re-validated against authoritative ref + body HEAD before reporting. |

## 10. Risks / open questions for review

1. **§3 lease-validity gate**: is there a cheap, correct "is the lease currently valid" signal `poolAccess` can
   read without racing the keeper, and does gating reads on it break any legitimate in-flight read that used to
   succeed during a brief renewal gap (a new throw where before there was none)?
2. **§6 guard**: is policy-membership the right discriminator; is skip-foreign ever unsafe (could it skip a disk
   that genuinely holds the dropped table's orphaned parts because the table's policy changed over time)? Is the
   unloaded-table conservative-retry acceptable, and are there other `getDisksMap()` sweeps that removing Part 6
   breaks (enumerate all callers)?
3. **§5 STOP**: is "refuse if in use" detection sound (how to enumerate all pool users beyond loaded
   `MergeTreeData` — `StorageSet`/`Log`/`StripeLog`/`Join`, reservations, in-flight backups), and is joining the
   keeper thread from the admin thread free of the C6 self-join under all races?
4. **§4 rollback**: does anything in the kept set (Part 1, poolAccess, startup, GC entry-point gating) secretly
   depend on `MountState`/`Unmounting`/`Dormant` such that collapsing to the lease gate changes behavior?
5. **§7 FSCK**: is `meta_without_body` the only hard class needing the ref+HEAD re-check, or do reachable/
   dangling/unaccounted also false-positive on a live concurrently-GC'd pool?
6. **Auto-recovery**: after re-acquiring a lease, is any cached state (writer epoch, GC leadership, ref table)
   left stale such that resuming is unsafe rather than transparent?
