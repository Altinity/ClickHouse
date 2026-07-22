# CAS disk: identity-validated recovery + honest throw-on-not-live (rev.2)

Status: **DESIGN rev.2 — for review.** Narrowed after a codex gpt-5.6-sol (high) adversarial review of rev.1
found 3 P0 blockers. Supersedes the whole runtime-reuse lifecycle line.

**What changed rev.1 → rev.2 (per the codex review):**
- **Auto-recovery is NOT transparent-and-safe (rev.1 P0 #1/#2).** The existing self-remount (`tryRemountOnce`,
  `CasPool.cpp:640-725`) re-claims owner on an *empty* subtree — so `rm -rf` under a mount makes it **bootstrap
  a fresh empty pool and serve it as healthy** (silent empty table, worse than a throw). rev.2's core is the fix
  for this: **identity-validated recovery** — validate `_pool_meta` before re-arming; missing ⇒ fail-loud. The
  fresh-incarnation recovery (new writer epoch etc.) **already exists** and is **kept**; rev.1 merely misdescribed
  it as "keeper re-acquires the old lease" — it does not.
- **Runtime `STOP`/`START` DROPPED (rev.1 P0 #3).** Safe STOP needs a process-wide disk-use RAII barrier
  (backups/reservations/buffers/non-MergeTree engines hold untracked `DiskPtr`s); out of scope. Only
  `GC STOP/START` remains.
- **Part 6 rollback = throw everywhere, "let it break" (owner decision).** An operation that touches a not-live
  CA disk fails, like touching an unmounted Linux filesystem. The **one** exception is the `DROP` finalizer
  fan-out (§5): it sweeps *all* disks for *every* table, so an unguarded throw wedges `DROP` of *unrelated*
  tables server-wide — that is not "let it break", it is one dead disk breaking things unrelated to it, and it
  re-breaks the test suite. That sweep gets minimal **typed** per-disk isolation (not benign-absent).
- **Access gate uses `Pool::mayMutate`** (local, atomic, TTL-hysteretic) — not `system.content_addressed_mounts`
  (remote I/O per call). No availability regression: a single slow renewal does not fence (rev.1 P0 (a) cleared).
- **Read-only CA disks have no lease** (rev.1 P1 #8): their access is not lease-gated — spelled out in §3.

Supersedes: `2026-07-22-cas-disk-self-quiesce-simplification-design.md` (Path B, blocked),
`2026-07-22-cas-disk-lifecycle-automount-design.md` (no code), and Parts 2–4 of
`2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md` (landed Dormant/UNMOUNT/MOUNT, Task 4–8).
**Part 1 (abort-hardening) is KEPT.** Companion: `2026-07-22-cas-disk-lifecycle-problem-and-constraints.md`.

---

## 1. The decision, in one paragraph

Do **not** build a runtime reuse lifecycle, and do **not** add benign-absent lies. A CA disk's data becoming
unreachable is handled by two honest mechanisms: (1) **identity-validated recovery** — the existing self-remount
must positively confirm the pool is the *same intact* pool before it recovers; if the backing was erased it goes
**fail-loud** ("data root erased"), never resurrects an empty pool; (2) **throw-on-not-live** — every access
(including probes) to a not-live CA disk throws, and callers that touch it fail honestly. The two remaining
runtime affordances are `SYSTEM CONTENT ADDRESSED GC STOP/START '<disk>'` and FSCK on a running disk. Tests are
fixed with unique disk **names** + `DROP ... SYNC` before any `rm -rf`. This directly satisfies the owner's
intent — "lease gone ⇒ throw, honestly; if data returns it heals; no benign-absent" — with **zero** new
lifecycle state and **one** small, typed, non-lying generic guard.

## 2. Identity-validated recovery (the core fix — A)

The self-remount `tryRemountOnce` (`CasPool.cpp:640-725`) exists to recover from a **fence-out by a twin**
(another node took the writer slot; *the pool data is intact*). It allocates a fresh writer epoch, quiesces &
re-recovers ref tables, and re-arms the fence — this **fresh-incarnation** sequence is correct and is **KEPT
verbatim**. Its bug: it calls `claimOwnerOrThrow` (which admits an **empty** root as a fresh bootstrap) without
first proving the pool it is recovering is the same pool it opened. So an erased backing (`rm -rf`) is
misclassified as "claimable empty root" and bootstrapped fresh.

**Fix:** before the recovery re-arms anything (and symmetrically at the startup mount-probe — the B200 startup
counterpart), **read/exact-HEAD `_pool_meta`** and require the **original pool ID and format**:

- `_pool_meta` present and matches the opened identity ⇒ this is a genuine fence-out / transient → proceed with
  the existing fresh-incarnation recovery.
- `_pool_meta` absent, or ID/format mismatch ⇒ the data root is **erased or foreign** → enter a typed,
  **fail-loud** `ContentAddressedDataRootErased` condition. Do **not** `claimOwnerOrThrow`, do **not** bootstrap.
  The disk stays not-live and every access throws (§3). Recovery of a genuinely-erased pool is a deliberate act
  (recreate the pool / restart), never an in-place silent re-bootstrap.

This turns `rm -rf`-under-mount from "silent empty healthy table" into an honest throw, while preserving
auto-recovery for real transient/fence cases. It is pure CA-internal; no generic/upstream surface.

## 3. Access gating: throw on not-live, no benign-absent

**Writable pool.** `poolAccess()` (`ContentAddressedMetadataStorage.cpp:~1548` / `throwNotMounted` `:805`) gates
on the pool's **local** admission signal `Pool::mayMutate` (`CasMountRuntime`, atomic lost/deadline fields with
TTL hysteresis) instead of the removed `MountState==Mounted` check. Lease currently valid → serve; lost / past
deadline / erased (§2) → throw `INVALID_STATE` (668) with a precise reason
("mount lease not currently held — backing may be temporarily unreachable" vs "data root erased — recreate the
disk"). Threads keep running so a transient outage auto-recovers (§2). A single slow renewal does **not** throw
(hysteresis), so there is no availability regression.

**Probes.** With Part 6 removed (§4), `existsFile`/`existsDirectory`/`existsFileOrDirectory`/`isDirectoryEmpty`/
`listDirectory`/`iterateDirectory`/`getStorageObjectsIfExist` **throw** on a not-live writable pool exactly like
content/size/mutation methods. No absent/empty answers anywhere.

**Read-only / observe-only pool** (`CasPool.cpp:282` — intentionally no mount keeper, no lease). Its access is
**not** lease-gated: reads are always admitted while the pool is published; a probe/read that hits a genuinely
vanished backing throws naturally from the backend 404 (honest, not benign-absent). It has no writer epoch and
does no destructive GC, so the mount-lease signal does not apply to it. `poolAccess` must branch on pool kind:
writable → `mayMutate` gate; read-only → published gate only.

**GC while lease invalid.** GC runs under its **own** `gc/state` lease (independent of the mount lease —
rev.1 P1 #10), and its destructive protocol is CAS/lease-protected and tolerant of stale leaders. rev.1's claim
"GC cannot act without the mount lease" was false and is dropped. GC remains 404-tolerant (record + continue,
never throw out of the fold). No new coupling is introduced; GC STOP/START (§6) is the only new control.

## 4. Rollback of the landed Task 4–8 — throw everywhere ("let it break")

Remove:

1. **`MountState` enum** (`Mounted`/`Unmounting`/`Dormant`) + every branch → collapse to the `mayMutate`/kind
   gate of §3. `poolAccess`/`throwNotMounted` kept, re-pointed.
2. **`SYSTEM CONTENT ADDRESSED UNMOUNT`/`MOUNT`** verbs + AccessTypes + `ASTSystemQuery` fields +
   `unmountSynchronously()` + the `Unmounting` drain. (No replacement — runtime STOP is dropped.)
3. **Part 6 benign-absent probes** (`ContentAddressedMetadataStorage.cpp:~557-564`, `:1114`, `:1370`, siblings) →
   throw via §3. **Load-bearing.**
4. **FSCK-dormant-only** (Task 7) → FSCK on a running disk (§7).
5. The **GC RUN → UNMOUNT → FSCK → rm -rf** test teardown → §8.

**Policy on the callers this breaks:** an operation that directly touches a not-live CA disk **fails, honestly**
— `SYSTEM UNFREEZE` (`Freeze.cpp:147`), `system.remote_data_paths` (`:281`), `SYSTEM CONTENT ADDRESSED GC RUN`
fan-out (`InterpreterSystemQuery.cpp:2456`), `ATTACH AS REPLICATED` cleanup (`InterpreterCreateQuery.cpp:2807`),
the MergeTree foreign-part sanity scan (`MergeTreeData.cpp:2398`), non-Atomic `DROP` (`DatabaseOnDisk.cpp:394`).
This is the intended contract, like operating on an unmounted Linux filesystem. We do **not** add per-caller
protection. (These are enumerated so reviewers know the blast radius is deliberate, not missed.)

Keep untouched: Part 1, the `poolAccess` snapshot, atomic-publish `startup()`, `ca-fsck` rename, the GC-round
entry-point gating fix.

## 5. The one exception: typed per-disk isolation in the `DROP` finalizer

`DatabaseCatalog::dropTableFinally` (`DatabaseCatalog.cpp:1657-1665`) sweeps **all** `getDisksMap()` disks for
**every** dropped table and re-queues the table forever on any throw (`dropTablesParallel` `:1589`). The drop
queue is **server-wide**, so one lingering not-live CA disk would wedge `DROP` of **unrelated** tables until
restart — and re-break the test suite. This is *not* "the operation that touched the dead disk fails"; it is a
dead disk breaking things that never used it. So this **one** sweep gets isolation — **not** a benign-absent lie
(the disk still throws); the *caller* is made resilient:

- Wrap the per-disk probe+remove in try/catch that catches **only** a typed `ContentAddressedDiskNotLive`
  exception, and **only after confirming the disk is CA** (all other exceptions propagate — rev.1 P1 #6, no
  swallowing partial-`removeRecursive`/permission failures).
- The typed exception carries the §2 distinction:
  - **Foreign** disk (not in the dropped table's storage volumes), any not-live reason → **skip + log**. (Its
    orphaned parts are never cleaned by CA's own orphan sweep regardless — CA lacks `supportsStat`/`supportsChmod`
    so `cleanupStoreDirectoryTask` `:1986` skips CA anyway — rev.1 P1 #4/#7; skipping here changes nothing.)
  - **Own** disk (in the table's volumes): `DataRootErased` → **skip + log** (nothing to reclaim — honest, the
    C2/R2 "positively-confirmed vanished ⇒ benign is truthful" case); transient-not-live → **re-queue** (durable
    deferral, never silent-skip → no leak; drains on recovery).
- Discriminating own-vs-foreign needs disk-ownership, which today only works for `MergeTreeData`
  (rev.1 P1 #5: `Log`/`StripeLog`/`Set`/`Join` cast to null → misclassified). Use the general
  `IStorage`→disks contract if one exists; if not, add a minimal `IStorage::getDisks()`-style accessor rather
  than casting to `MergeTreeData`. (Open item — §9.)

Because tests DROP-while-live (§8), a table's **own** disk is live at finalize time in the normal path, so the
own-disk branches are the rare involuntary cases; the common effect of this guard is "skip a lingering foreign
dead disk", which is exactly what unblocks unrelated `DROP`s.

## 6. `SYSTEM CONTENT ADDRESSED GC STOP '<disk>'` / `GC START '<disk>'`

Stop/restart **only** the GC scheduler; the mount keeper and normal disk access are untouched. `stop` must
**clear `i_am_leader`** (rev.1 P1 #10) so introspection does not lie and a later `GC START` re-enters leadership
via a fresh round, and must be genuinely restartable (`CasGcScheduler`, same instance, `gc_id` preserved).
`start`/`stop` are serialized (externally linearizable) so concurrent admin calls cannot race the scheduler
lifecycle. This is the granular "turn GC off around a test / maintenance window" handle the owner asked for.

## 7. FSCK on a running disk

Drop dormant-only; FSCK runs on a running disk with concurrent (any-node) GC expected. The existing blob
`Dangling` path is **already sound** (it exact-HEADs the blob then re-replays refs via `blobStillReferenced`,
`CasFsck.cpp:370`). Apply the **same** revalidation to the two remaining hard classes before incrementing hard
counters: **missing committed manifest** (`CasFsck.cpp:290`, currently reported from a possibly-stale recovered
table) and **`meta_without_body`** (`:563`, currently from two raw LIST partitions while GC deletes body-then-
meta, `CasGc.cpp:419/474`) — re-validate against a fresh authoritative ref view **and** a direct HEAD of the
exact object. `Reachable` is informational and `Unaccounted` is not part of `clean`; codex confirmed no other
hard class needs this. One-row summary output unchanged.

## 8. Tests

- Rewrite `05020_content_addressed_fsck.sh` + the `04290`/`04295` family: unique per-invocation disk **names**
  (`${CLICKHOUSE_TEST_UNIQUE_NAME}`, the registry keys on **name** — C8) and paths
  (`${CLICKHOUSE_USER_FILES_UNIQUE}`); update every SQL predicate referencing the old fixed name.
- `DROP TABLE ... SYNC` (or `database_atomic_wait_for_drop_and_detach_synchronously=1`) **before** any `rm -rf`,
  so the finalizer reclaims on a **live** disk. With §2, the lingering keeper after `rm -rf` cannot resurrect the
  erased pool — it fail-loud-throws and logs (Part 1 keeps it from aborting), and unique names prevent reuse — so
  **no `STOP` is needed** to make teardown safe (rev.1 P1 #12 is resolved by §2, not by STOP).
- Never `rm -rf` a globally-configured standard CA disk's data — only unique-name per-test disks.
- New tests: `GC STOP/START` (no new rounds after STOP; reads/writes still work; rounds resume after START);
  the DROP-wedge guard (§5) — table A on a CA disk erased under it, table B on a normal disk; `DROP B` completes
  though the dead CA disk lingers; identity-gate (§2) — erased backing ⇒ access throws, never empty result
  (gtest/integration).

## 9. Open questions for review

1. **§2 identity gate**: is exact-HEAD of `_pool_meta` + ID/format the right, race-free discriminator between
   "fenced-but-intact" and "erased/foreign", and does it also need to run at the startup mount-probe (B200)?
   Any window where meta is transiently unreadable but data is intact (would wrongly fail-loud)?
2. **§5 own-vs-foreign**: is there an existing `IStorage`→disks ownership contract, or must one be added; is
   "foreign not-live ⇒ skip" ever a real leak beyond the already-never-swept CA orphan case?
3. **§3 read-only branch**: is "published gate only, throws on backend 404" a complete and safe contract for
   read-only CA disks, including their (absent) GC-safety story?
4. **§6 GC STOP/START**: does clearing `i_am_leader` on stop + fresh-round re-entry fully avoid a stale-leader
   destructive action across a STOP/START, given GC's independent `gc/state` lease?
5. **§4 rollback**: does collapsing `MountState` to the `mayMutate`/kind gate change any behavior the kept
   pieces (Part 1, poolAccess snapshot, startup, GC entry-point gating) relied on?
6. **§2 recovery vs GC-fence**: after identity-validated recovery re-arms with a fresh epoch, is any cached
   state (ref table, GC leadership) left stale such that resuming corrupts — or does the existing
   quiesce/re-recover sequence fully cover it?
