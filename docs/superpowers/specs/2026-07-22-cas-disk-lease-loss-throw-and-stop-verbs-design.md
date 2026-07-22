# CAS disk lifecycle rev.3: throw-when-uncertain, truth-when-proven (`Vanished`)

Status: **DESIGN rev.3 — for review.** Rewrites rev.2 in place (git history holds rev.1/rev.2).

**The rev.2 → rev.3 change, in one sentence:** stop trying to fix the not-live-disk problem at the *callers*
(rev.1's typed-catch guard, the eject idea) — the correct behavior depends **only on the disk's own state**,
so encode it there: **transient-not-live ⇒ throw to everyone; positively-proven-erased (`Vanished`) ⇒ answer
the truth (absent/empty/no-op) to everyone**. The own-vs-foreign disk discriminator, the DROP-finalizer guard,
the `IStorage`→disks contract, and registry eviction all **dissolve**. Generic/upstream code changes: **zero**.

Why rev.2's §5 died: the owner rejected caller-side skip-with-log as "Part 6 again" (a quiet noop), and they
were right — *any* caller-side skip policy re-encodes the same ambiguity. Why eject died on formulation:
(a) a loaded table re-resolving its storage policy after eviction reaches `getOrCreateDisk` → fresh disk →
`Pool::open` on the erased root → **bootstraps an empty pool** → silent empty table through a new door (the
P0 #1 class again); (b) a table whose own disk was ejected-but-throwing becomes permanently un-droppable
(`drop()` throws forever). Keeping the husk registered and giving it truthful `Vanished` semantics avoids both.

**Part 6 vs `Vanished` — the honesty distinction this whole design rests on:**

| | Part 6 (rolled back) | `Vanished` (rev.3) |
|---|---|---|
| Gate condition | "not Mounted" — **ambiguous**, data possibly intact | identity-gate **proved** the data root is erased/replaced |
| exists/list answers | absent/empty — **a lie** when data is intact | absent/empty — **the truth** |
| Content reads | absent-ish | **typed throw** ("data root erased") — loud diagnosis |
| Writes/creates | benign | **typed throw** |
| Transient outage | lied "absent" | **throws** (no benign answer while uncertain) |

This is exactly constraint C2 / requirement R2 of the companion doc
(`2026-07-22-cas-disk-lifecycle-problem-and-constraints.md`): benign-absent is legitimate **only** in a
positively-established data-erased state — and rev.3 finally builds that establishment.

Supersedes: rev.1/rev.2 of this file, `…-self-quiesce-simplification-design.md` (Path B),
`…-lifecycle-automount-design.md`, Parts 2–4 of `2026-07-21-cas-mount-lease-abort-…-design.md` (landed
Task 4–8; **Part 1 abort-hardening KEPT**).

---

## 1. State model and access semantics

Per writable pool, one runtime condition with three values (an atomic enum on the pool runtime, no persisted
state):

```
[Running/live] ──lease renewal fails──► [Running/transient-not-live] ──identity gate proves erased/replaced──► [Vanished]
      ▲                                        │   (self-remount keeps retrying;                                (terminal)
      └────────── self-remount succeeds ───────┘    ALL access throws)
```

| Operation class | Running/live | transient-not-live | `Vanished` |
|---|---|---|---|
| exists/isDirectoryEmpty probes | real answer | **throw** 668 ("lease not held; backing may be temporarily unreachable") | `false`/`true-empty` (truth) |
| listDirectory/iterateDirectory/getStorageObjectsIfExist | real answer | **throw** 668 | empty (truth) |
| Content reads (readFile/getFileSize/…) | real answer | **throw** 668 | **throw** typed `CAS_DATA_ROOT_ERASED`-style 668 ("data root erased/decommissioned — recreate the disk; restart re-registers the name") |
| Removes (removeFile/removeRecursive/…) | real | **throw** 668 | **no-op success** (truth: nothing to remove) — this is what lets `DROP` of a vanished-disk table complete |
| Writes/creates/renames | real | **throw** 668 | **throw** typed erased error |
| `store()` (admin ops, GC RUN) | real | throw | throw typed erased error (GC RUN reports per-disk status, no silent no-op) |

- **Transient-not-live** covers *every* unproven cause: lease renewal failure, fence-out by a twin, network
  outage, 403/5xx, mid-recovery. Reads are gated too (the mount contract couples any pool use to a live
  lease — C3; this is a deliberate tightening and is called out for review in §10).
- The gate signal is the pool's local atomic fence (`Pool::mayMutate` machinery in `CasMountRuntime` —
  atomic lost/deadline with TTL hysteresis; codex rev.1 #9 confirmed a single slow renewal does **not**
  fence, so no availability cliff on a heartbeat blip). Not `system.content_addressed_mounts` (remote I/O).
- **Read-only pools** (`CasPool.cpp:282` — no keeper, no lease) are out of scope of the gate: they keep
  today's behavior (backend 404s surface naturally; the `DROP` finalizer sweep already skips read-only disks
  via `!disk->isReadOnly()` at `DatabaseCatalog.cpp:1650`, so they cannot wedge anything).

## 2. The identity gate (single choke point: `tryRemountOnce`)

The existing self-remount (`CasPool.cpp:640-725`) is the **only** recovery path after the keeper exits on a
terminal renewal failure, and it is **kept verbatim** (fresh writer epoch → `quiesceRefTablesForRemount` →
`setLiveWriterEpoch` → `armMountFence`; codex rev.1 P0 #2). rev.3 adds **step 0**, before
`claimOwnerOrThrow`:

Authoritative, cache-bypassing GET of `_pool_meta`, compared against the in-memory `PoolMeta` identity
captured at `open`:

1. **Present + identity matches** → genuine fence-out/transient → proceed with the existing recovery.
2. **Present + identity mismatch** (different pool id/format — someone recreated a pool under our root) →
   **`Vanished(replaced)`** immediately (an authoritative read proving foreign content; no second sample
   needed).
3. **Clean, authoritative 404** → count it; **two consecutive** clean 404s (across two remount attempts at
   the loop's cadence) → **`Vanished(erased)`**. A single 404 → return false, retry.
4. **Any error** (timeout, 5xx, 403, connection reset) → **reset the 404 counter**, stay
   transient-not-live, retry forever. **An erased verdict is never derived from errors** — this is what makes
   a transient outage unable to falsely kill a healthy disk (the Path-B blocker #1 class), per C2.

Notes: `claimOwnerOrThrow`'s empty-root bootstrap admission is thereby unreachable from recovery — the gate
answers first (fixes codex rev.1 P0 #1: `rm -rf` under a live mount can no longer be resurrected as a fresh
empty healthy pool). The startup-side cold `open` is intentionally **not** gated: a cold open of an empty
root is the documented way pools are ever created (§8, known limits).

## 3. On entering `Vanished`

- Set the terminal flag (atomic; checked by the access layer per §1's table).
- The remount thread logs **one** WARN ("CAS data root for '<disk>' is erased/replaced — disk is vanished;
  reads/writes fail, removals no-op; recreate the pool and restart to reuse the name"), bumps a
  `CasDataRootVanished` ProfileEvent, and **exits its own loop**. The GC scheduler observes the flag at its
  next tick and exits likewise. The keeper already exited (terminal renewal). No thread joins another from a
  callback — joins happen only in `~Pool` at destruction (C6 holds). G2 (no zombie spam) is satisfied: after
  the one WARN, silence.
- The disk **stays registered** (no eject): `getDisksMap` sweeps get truthful answers, loaded tables get the
  typed erased error, `system.content_addressed_mounts` can still show it (§7), and re-`CREATE` with the
  same name gets the husk's honest throw rather than a silently re-bootstrapped pool. The husk costs one
  quiesced object until restart (the known, accepted disk-registry leak).

## 4. Blast radius (deliberate, enumerated)

With Part 6 rolled back, callers touching a **transient-not-live** CA disk fail honestly ("пусть ломает" —
like touching a dead NFS mount). Enumerated so review sees this is deliberate:

| Caller | transient-not-live | `Vanished` |
|---|---|---|
| `DROP` finalizer all-disk sweep (`DatabaseCatalog.cpp:1657`) | per-table throw → re-queue → **auto-drains on recovery** (bounded delay, not a wedge; the queue's existing per-table catch does the deferral — no new code) | exists=false → skip (truth) |
| `table->drop()` of a table on the disk | throws → that table's `DROP` re-queues until recovery | removes no-op → **`DROP` completes** |
| Orphan store-dir sweep (`:1986`) | already skips CA disks (`supportsStat`/`supportsChmod` false) — unaffected either way | same |
| `SYSTEM UNFREEZE` (`Freeze.cpp:147`), `system.remote_data_paths` (`:281`), `ATTACH AS REPLICATED` cleanup (`InterpreterCreateQuery.cpp:2807`), non-Atomic `DROP` (`DatabaseOnDisk.cpp:394`) | honest error; retry later | truthful absent/empty → proceed |
| MergeTree foreign-part scan (`MergeTreeData.cpp:2398`) | honest load error for tables whose `search_orphaned_parts_disks` reaches the disk (defaults to be verified at implementation) | truthful absent → proceeds |
| `SELECT` on a loaded table of the disk | throws 668 | typed erased error (loud — **never** a silent empty result; enumeration-based empty loads can't happen because ATTACH/CREATE hit the write-throw at directory init first, making the previously-"fragile accident" ordering an explicit contract) |
| `BACKUP` of such a table | throws | typed erased error (no silent empty backup) |

**Residual hole + its escape hatch:** a disk that is *permanently* transient-not-live but never yields a
clean 404 (e.g. credentials broken forever) delays all `DROP`s indefinitely (each re-queues). Cure below.

## 5. `SYSTEM CONTENT ADDRESSED FORGET '<disk>'`

The operator's fire-marshal verb: an explicit, logged assertion "this pool is gone/decommissioned" that
force-transitions the disk to `Vanished` (same semantics, threads stop the same way). It is the escape hatch
for the §4 residual hole and the decommission handle that replaced the unsound runtime `STOP` (codex rev.1
P0 #3 — no in-use registry needed: nothing is torn down; the pool object stays; in-flight users start
getting `Vanished` answers). If the operator is *wrong* (data intact), `FORGET` reintroduces the benign-lie
by explicit human assertion — documented as destructive-intent, like `umount -f`; the log screams. Access
control: its own `AccessType` under `SYSTEM`.

Deliberately **not** doing: runtime `STOP/START` (unsound without a process-wide disk-use barrier),
`UNMOUNT/MOUNT` reuse (rolled back), registry eviction.

## 6. `SYSTEM CONTENT ADDRESSED GC STOP '<disk>'` / `GC START '<disk>'`

Unchanged from rev.2: stop/restart only the GC scheduler; disk fully usable meanwhile. `stop` **clears
`i_am_leader`** (codex rev.1 #10) and is genuinely restartable (same instance, `gc_id` preserved);
`start`/`stop` serialized. GC's own `gc/state` lease is independent of the mount lease (rev.1's "GC cannot
act without the mount lease" claim was false and stays dropped); its destructive protocol is CAS/lease
protected and stale-leader tolerant.

## 7. Introspection

`system.content_addressed_mounts` gains a non-`store()`-gated lifecycle snapshot source (codex rev.1 #11):
name, lifecycle (`live` / `not_live` / `vanished(reason, since)`), last-known identity. `Vanished` disks
remain visible (an advantage of keeping the husk registered).

## 8. Known limits (explicit non-goals)

- **Cross-session:** a server restart over an erased root re-bootstraps an empty pool and tables load empty
  — exactly today's behavior; indistinguishable from a legitimately new pool without durable local state.
  Candidate future work: a local marker enabling a boot-time warning. Out of scope.
- **Partial tampering** (e.g. `rm` of only refs, `_pool_meta` intact): invisible to the lease/identity gate;
  FSCK's domain. Out of scope.
- **Permanently-unconfirmable disk**: visible `DROP` delays until `FORGET`/restart (§4/§5).

## 9. Rollback, FSCK, tests

**Rollback of landed Task 4–8** (as rev.2, restated): remove the `MountState` enum and branches (collapse to
§1's gate), the `UNMOUNT`/`MOUNT` verbs + AccessTypes + `ASTSystemQuery` fields + `unmountSynchronously` +
drain loop, FSCK-dormant-only, and the GC RUN→UNMOUNT→FSCK→rm teardown pattern. The **Part 6 probe sites are
not deleted but re-keyed**: gate changes from `!isMounted()` (ambiguous) to the `Vanished` flag (proven),
and their write/content-read siblings get the typed erased throw — per §1's table; everything
transient-not-live throws. Keep: Part 1, `poolAccess` coherent snapshot, atomic-publish `startup()`,
`ca-fsck` rename, GC-round entry-point gating.

**FSCK on a running disk** (as rev.2): drop dormant-only; before incrementing hard counters, re-validate
**missing committed manifest** (`CasFsck.cpp:290`) and **`meta_without_body`** (`:563`) against a fresh
authoritative ref view **plus** an exact-object HEAD (the already-sound blob-`Dangling` pattern,
`CasFsck.cpp:370`), because GC deletes body-then-meta (`CasGc.cpp:419/474`). No other hard class needs it
(codex-confirmed). One-row summary unchanged.

**Tests:**
- `05020` + `04290`/`04295` family: unique per-invocation disk **names** (`${CLICKHOUSE_TEST_UNIQUE_NAME}` —
  registry keys on name, C8) and paths (`${CLICKHOUSE_USER_FILES_UNIQUE}`); update all SQL predicates;
  `DROP TABLE ... SYNC` **before** any `rm -rf`; never `rm -rf` a globally-configured disk's data.
- New: identity gate — erase pool under a live mount ⇒ disk becomes `Vanished`; `SELECT` fails with the
  typed erased error (never empty); `DROP` of that table **completes**; `DROP` of an unrelated table
  completes; one WARN, then log silence (G2).
- New: transient — block/unblock backing (or drop lease key only, restore) ⇒ access throws during the gap,
  auto-recovers after; a `DROP` issued during the gap drains after recovery.
- New: `FORGET` — force-vanish; same observable contract. `GC STOP/START` — rounds stop/resume; access
  unaffected; leader flag drops on stop.

## 10. Open questions for review

1. **Truthful-absent completeness:** enumerate every reader of the `Vanished` surface — is there any path
   where truth-empty still yields a silent wrong result (fresh enumeration flows: ATTACH really blocked by
   the write-throw at directory init in all variants? detached-parts scans? backup logic that treats
   "no files" as success)?
2. **404-confirmation soundness** vs real backends: versioned buckets/delete markers (GET-after-`rm -rf`
   semantics on S3/GCS/rustfs), strong-read guarantees, the 2-consecutive-404 threshold, resetting on
   interleaved errors.
3. **Reads gated in transient-not-live:** today reads are effectively *not* fence-gated (`mayMutate` gates
   mutations). Rev.3 tightens reads to throw during a lease gap — contract-correct per C3, but is it an
   acceptable availability change, and is the fence's TTL-hysteresis window sufficient to keep normal
   operation unaffected?
4. **Remove-as-no-op:** any caller that interprets remove success as an accounting/space-freed signal in a
   way that no-op breaks?
5. **`FORGET`:** should it also run the keeper's terminal release op (free the mount slot for other nodes)
   or leave the slot to expire? Operator-wrong-case consequences acceptable?
6. **Thread-exit races:** remount thread setting `Vanished` and exiting vs a concurrent GC tick / `~Pool` /
   a `FORGET` racing natural detection.
7. **Residual `DROP` delay** under permanently-transient: acceptable operationally (log rate, queue growth),
   given `FORGET`/restart as cures?
8. **Rollback coupling:** does collapsing `MountState` to this gate change behavior any kept piece (Part 1,
   `poolAccess`, startup, GC entry-point gating) relied on?
