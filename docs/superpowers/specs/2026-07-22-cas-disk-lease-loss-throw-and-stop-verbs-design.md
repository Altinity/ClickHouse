# CAS disk lifecycle rev.7: throw-when-uncertain, truth-when-proven

Status: **DESIGN rev.7 — final.** Rewrites rev.6 in place (git history holds rev.1–rev.6).

**rev.6 → rev.7 (per the codex closing verification — C3/C5 faithful, consolidation confirmed clean, the
Local `05020` story walk-through passed):** four final narrow amendments, marked **[D1]–[D4]** inline:
[D1] the erasure-proof window requires FULL durable-lane quiescence (outstanding-durable-request counter,
GC exit, max-timeout grace) — "ref lanes OR 30 s" was insufficient; [D2] the zero-write bootstrap is
ordered against the mutating `_probe` capability battery (+ `pool_prefix` declared exclusively CAS-owned);
[D3] matching sentinel reappearance (backup restore) does NOT auto-revive `IdentityLost` — restart is the
recovery; [D4] companion R4/§8 aligned (accepted `IdentityLost` fan-out exception recorded; the "open
core" converted to a disposition table), the baseline acceptance matrix restored, and the `GC RUN`
`pending_*` columns named in the keep-list.

**rev.5 → rev.6 (per the codex rev.5 verification — 6 of 9 fold items were confirmed sound: B3, B4, B6,
FSCK-advisory, fail-closed teardown, the FORGET deadlock fix):** five narrow amendments **[C1]–[C5]** plus
cross-reference cleanups. The architecture ("uncertainty throws; genuinely proven old-generation loss
permits truthful absence") has survived three adversarial reviews unchanged.

- **[C1]** `IdentityLost` is fail-loud but **observationally non-absorbing**: a progressive `rm -rf`
  deletes the sentinels first — terminal absorption would strand the disk before the emptiness proof could
  complete. A low-rate, non-mutating lifecycle observer remains; one-way `IdentityLost → Vanished(erased)`.
- **[C2]** The "all commits are fence-gated" safety claim was **false** (`CasPlainObjects::casPutObject`
  writes table-level verbatim/dedup/rename objects with no `mayMutate` check). Every durable-effect path
  gets a fence-generation check; the emptiness-proof window opens only after **full durable-lane
  quiescence** ([D1], §2 — ref lanes settled AND the outstanding-durable-request counter is zero AND the
  GC scheduler/round exited AND the minimum grace elapsed).
- **[C3]** Natural `Vanished(erased)` requires a **strong-LIST backend capability** (AWS S3, GCS —
  documented; RustFS pending evidence; Local/Emulated/unqualified gateways — never): elsewhere the only
  path to benign truth is `FORGET`.
- **[C4]** Startup bootstrap of a **missing `_pool_meta` requires a provably empty prefix**; a non-empty
  prefix without meta fails startup with zero writes (closes the restart-poisons-partial-pool hole and
  hardens the cross-session limit).
- **[C5]** Honest wording: one `IdentityLost` disk stalls **all** eligible Atomic `DROP` finalizations
  (default `search_orphaned_parts_disks=ANY` sweeps every disk) until `FORGET`/restart — accepted and
  stated, not narrowed to "drops involving the disk".

**Part 6 vs the terminal states — the honesty distinction this design rests on:**

| | Part 6 (rolled back) | `Vanished` (rev.6) |
|---|---|---|
| Gate condition | "not Mounted" — **ambiguous**, data possibly intact | erasure **proven** (§2) or operator-asserted (`FORGET`) |
| exists/list answers | absent/empty — **a lie** when data is intact | absent/empty — **the truth** |
| Content reads | absent-ish | **typed throw** ("data root erased") — loud diagnosis |
| Writes/creates | benign | **typed throw** |
| Transient outage / `IdentityLost` | lied "absent" | **throws** (no benign answer while uncertain) |

This is constraint C2 / requirement R2 of the companion: benign-absent is legitimate **only** in a
positively-established data-erased state.

Supersedes (all **deleted** from the tree — git history holds them): rev.1–rev.5 of this file, the Path-B
self-quiesce spec, the automount/eject spec, and the 2026-07-21 Dormant/UNMOUNT spec (its landed Task 4–8
are rolled back per §9; its **Part 1 is KEPT** — contract transferred into §9). The living documents are
this file and the companion `2026-07-22-cas-disk-lifecycle-problem-and-constraints.md` (C2/C3 refined in
step with this revision).

---

## 1. States and the operation-class gate

```
[Running/live] ──lease lost──► [transient-not-live] ──┬─ sentinels gone, prefix NOT empty ─────────► [IdentityLost]
      ▲                             │ (all access throws;  │                                        (fail-loud; bg writers stopped;
      └── self-remount succeeds ────┘  remount retries)    │                                         non-mutating observer stays [C1])
                                                           │                                                    │ one-way, on proof
                                                           ├─ pool-wide emptiness proof (§2) ──────► [Vanished(erased)]   (truth)
                                                           └─ _pool_meta present, foreign pool_id ─► [Vanished(replaced)] (truth)
                       FORGET (from any state) ────────────────────────────────────────────────────► [Vanished(forgotten)] (truth)
```

(Diagram note, Task-5 review 2026-07-23: `IdentityLost → Vanished(replaced)` is also reachable — a
foreign `_pool_meta` appearing while `IdentityLost` proves our generation gone just as authoritatively as
from transient. Only an out-of-contract erase-and-recreate can produce it; the transition is truthful and
terminal, so it is allowed.)

Plus the minimal storage-level lifecycle `Constructing → Started → ShutDown`: outside `Started`, the
null-pool check in the `poolAccess` coherent snapshot fails loud.

**Operation classes — SIX**, enforced centrally at every public metadata/transaction entry (the known
short-circuit offenders — `liveTreeDirHasChildren` `ContentAddressedMetadataStorage.cpp:883`,
`isDirectoryEmpty` `:1385`, no-op `createDirectory*` `ContentAddressedTransaction.cpp:924`, empty-txn
commit `:408`, `setLastModified`/`setReadOnly` `:1109`, the `tryGetInManifestBytes` catch-all `:1488` to be
narrowed — all routed through the gate). The implementation plan must publish the complete method→class
inventory.

| Class | live | transient / `IdentityLost` | `Vanished` |
|---|---|---|---|
| **Factory / capability / introspection** (`createTransaction` — I/O-free, verified; `getType`/`getPath`/capability getters/`gcHealth`/lifecycle snapshot) | works | **works** | **works** |
| Probe / enumeration | real | **throw** 668 | absent / empty (truth) |
| Content read | real | **throw** 668 | **throw** typed, message names the ACTUAL reason (see below) |
| Create / write / rename (incl. previously-no-op sites; empty-txn commit) | real | **throw** 668 | **throw** typed erased |
| Remove | real | **throw** 668 | **no-op success** (truth) — a vanished-disk table's `DROP` completes |
| Admin (`store()`, GC RUN) | real | throw | throw typed erased (per-disk status in fan-outs) |

**Error messages tell the truth about the reason [D5].** The `Vanished` typed error (one error code) carries
the sub-state in its message, because a wrong message would mislead diagnosis — the exact failure mode this
design exists to kill:
- `Vanished(erased)` — "data root erased (verified: pool prefix empty) — recreate the disk; restart
  re-registers the name". Erasure was *proven*; saying so is the truth.
- `Vanished(replaced)` — "data root replaced by a foreign pool (pool_id mismatch)". Our generation is gone,
  but the prefix is not empty — never claim "erased".
- `Vanished(forgotten)` — "disk decommissioned by SYSTEM CONTENT ADDRESSED FORGET at <timestamp> — erasure
  was NOT verified; if this was a mistake the data may be intact (restart re-registers the name)". `FORGET`
  is an operator assertion, not a proof; the message must say so, so an operator-wrong case is diagnosable
  from the first error line and traceable to the audit log.
The same reason strings appear in the `system.content_addressed_mounts` lifecycle snapshot and the one
WARN logged at the transition.

**Gate lifetime [C2]:** the gate governs **admission**, and additionally **every durable-effect
finalization is fence-generation-checked**: the plain-object surface (`CasPlainObjects::casPutObject` /
delete — table-level verbatim files, dedup metadata, rename copy/remove,
`ContentAddressedTransaction.cpp:748/:1312`) and staging-buffer finalize acquire a fence-generation token
at admission and re-check it immediately before the durable CAS/PUT; a mismatch aborts the write with the
typed transient error. The ref-log publish chokepoint is `appendRefOps` + the controlled staging methods
(not `mutateShard` — rev.5's wording corrected), already fence-coupled. Admitted, non-durable in-flight
**reads** may complete on their existing pipelines (their failure modes are the backend's own errors) —
this is the C3 refinement in the companion.

**The empty-proof rule** (confirmed sound by the rev.5 review): pre-terminal and on read-only pools, an
enumeration about to answer empty at a data root (`TableDir`, `DetachedContainer` `DirShape`s,
`ContentAddressedMetadataStorage.cpp:1303`) first confirms `_pool_meta` with an authoritative, uncached
probe; absent ⇒ typed throw. Terminal `Vanished` answers truth-empty directly. Enumeration latency now
couples to one backend GET on the empty path — the correct fail-closed trade-off, bounded by ordinary
request timeouts.

Why this rule is load-bearing for **read-only** pools: they have no keeper (no lease, no natural
detection), and MergeTree skips both directory creation and the `format_version.txt` write on a read-only
disk (`MergeTreeData.cpp:469/:501`) — enumeration is their *only* line of defense against silently
attaching an empty table over an erased backing. On **writable** pools ATTACH/CREATE are protected by the
write class instead: directory creation is unconditional for CREATE and ATTACH alike
(`need_create_directories = true` — trace-verified in the automount exploration), so the gated
`createDirectory*`/`format_version` write (`MergeTreeData.cpp:513`) throws before `loadDataParts` ever
enumerates — an explicit contract now, not the fragile ordering accident it used to be.

## 2. Typed erasure evidence

Single choke point: step 0 of `tryRemountOnce` (existing fresh-incarnation recovery kept verbatim;
`claimOwnerOrThrow`'s empty-root bootstrap unreachable from recovery).

`ProbeResult ∈ {Present(meta), KeyAbsent, ContainerAbsent, AccessDenied, Indeterminate}` implemented at the
`IObjectStorage`/raw-error boundary (raw HEAD error preserved before `getObjectInfo.cpp:135` discards it —
today S3 merges `NO_SUCH_KEY`/`NO_SUCH_BUCKET`/`RESOURCE_NOT_FOUND` into one `nullopt`; Local: explicit
stat of the configured container/parent distinguishes deleted-prefix from missing-key). Container proof =
`ListObjectsV2(max-keys=1, prefix=pool_root)`, not bucket HEAD. The IAM permutations (verified against AWS
semantics in the rev.4 review):

| IAM state | Probe outcome | Verdict |
|---|---|---|
| `ListBucket(prefix)` + `GetObject` allowed | absence yields a genuine 404 | trustworthy `KeyAbsent` |
| LIST allowed, GET denied | key probe answers 403 | `AccessDenied` → stay transient |
| LIST denied, GET allowed | container proof fails | stay transient |
| LIST denied, key absent | AWS answers 403, not 404 | stay transient |
| Gateway masking denied-GET as 404 | indistinguishable from absent | excluded by contract (capability-probe territory) |

- **`Present` + identity match** (`pool_id` + `blob_header_len`; format gate = successful compatible
  decode) → existing recovery. **This rule applies only in the transient state** (the live remount loop).
  **`Present` + foreign `pool_id`** → `Vanished(replaced)`.
- **Sentinels absent, prefix non-empty** → **`IdentityLost`**: writers stopped (the operation gate) and the
  keeper already exited; the GC scheduler THREAD keeps ticking but its effects are contained — its
  `gc/state`/heartbeat keys live under the pool prefix so any landed write resets the emptiness proof via
  the LIST, `round_in_flight` is held for the whole round, and the `has_observation` guard refuses to
  recreate an absent `gc/state` (wording corrected 2026-07-23 per the Task-8 review — the earlier "GC
  stopped" claim was neither implemented nor necessary). **The remount
  thread demotes to a lifecycle observer [C1]** — non-mutating, low-rate (renewal-period cadence with
  backoff), running only this typed probe; it never claims, allocates, or writes. One-way transition to
  `Vanished(erased)` when the proof below completes; `FORGET` and restart remain available. (This is how a
  progressive `rm -rf` — sentinels deleted first, the rest still draining — ends in `Vanished(erased)`
  instead of being stranded.) **Matching sentinel reappearance does NOT auto-revive [D3]**: if a backup
  restore brings `_pool_meta`/owner back with matching identity while the disk is `IdentityLost`, the
  observer stays non-mutating and the state stays fail-loud — auto-revival would trust a possibly-partial
  restore and violate the observer's contract. Recovery from a restore is a server **restart** (re-running
  the full mount/claim chain against the restored state).
- **`Vanished(erased)` proof [C2][C3]:** requires ALL of —
  1. the backend declares the **strong-prefix-LIST capability** [C3]: documented strongly-consistent LIST
     (AWS S3, GCS). RustFS: off until explicit evidence or an integration contract-test; Local/Emulated:
     **never** (Local listing is best-effort `LocalObjectStorage.cpp:450`, and an unmounted NFS/removable
     mount makes intact data look erased; Emulated scans before locking `CasObjectStorageBackend.cpp:1024`).
     Without the capability, `IdentityLost` is the terminal natural state and `FORGET` is the only path to
     benign truth — which is exactly the stateless-test story anyway (their teardown FORGETs).
  2. **full durable-lane quiescence [D1]** before the first sample — "ref lanes settled OR 30 s grace" is
     insufficient (an already-issued plain-object/staging/manifest request that passed its last token check,
     or an in-flight GC round writing `gc/state`/heartbeat objects (`CasGc.cpp:2418/:2442`), can land a
     durable write AFTER two empty samples). The proof window opens only when ALL of: ref lanes settled;
     the **outstanding-durable-request counter** (incremented at admission of every durable-effect
     operation, decremented at resolution — the same fence-token plumbing) is zero and stays zero across
     both samples; NO GC round is in flight at either sample (`isQuiescent` — the scheduler THREAD deliberately
     keeps ticking through `IdentityLost` [it exits at its next wake only once fully `Vanished`, §3], its
     writes contained by the LIST-reset + `round_in_flight` + `has_observation` backstops — wording aligned
     with the as-built rev-t8-adjudicated form, 2026-07-23); and a minimum grace ≥
     max(materialization grace, the backend's total request-timeout budget) elapsed since the fence trip;
  3. container proof succeeds AND the full-prefix LIST returns **zero objects**, in ≥2 cycles spaced ≥ the
     renewal period; any error or non-empty result resets the counter.
- **Startup [C4], ordered vs the capability probe [D2]:** `PoolMeta::createOrValidate` may **create** a
  missing `_pool_meta` only after proving the pool prefix empty (same full-prefix LIST; on non-strong-LIST
  backends a single authoritative check — best effort against the weaker guarantee, still fail-closed on
  any object found). Today the mutating `_probe/<random>/` capability battery runs BEFORE
  `createOrValidate` (`CasPool.cpp:230`, crash debris accepted-by-design `:245`, meta validated only at
  `:263`) — incompatible with "zero writes" as-is. **Bootstrap sequence**: (0) the zero-write residual
  check runs FIRST, before any probe write; it ignores structurally-valid `_probe/` debris (crash
  leftovers, a concurrent fresh opener's battery) when deciding whether CAS data/control state exists;
  (1) only then the capability battery; (2) then `createOrValidate`. Missing meta over a non-empty
  (non-`_probe`) prefix ⇒ startup fails with a typed error and zero non-probe writes. **`pool_prefix` is
  exclusively CAS-owned** — bootstrapping over unrelated foreign objects is rejected, not a supported
  layout. Restart is thereby a safe cure only after deletion completed; it can no longer mint a fresh
  identity over residual data.

**Contract [A1, wording fixed]:** *erase-and-**recreate*** at the same prefix while any old process may
live is out of contract (keys are not `pool_id`-namespaced; an open fence has up to ~TTL of write
authority). A plain live erase without recreation is *handled* (transient → `IdentityLost` →
`Vanished(erased)`/`FORGET`), not encouraged. `FORGET` is **node-local** (per `server_root_id`,
`CasLayout.h:295`) — never authorization to erase or reuse shared backing; same-prefix reuse requires
stopping/fencing every member first.

## 3. Terminal transitions; serialization

- One WARN + ProfileEvent (`CasIdentityLost` / `CasDataRootVanished`); keeper already exited; GC scheduler
  exits at its next tick; the remount thread either demotes to observer (`IdentityLost`) or exits
  (`Vanished`). **Joins: natural transitions defer joins to `~Pool`; `FORGET` joins keeper/remount/GC
  synchronously, outside `remount_mutex`, before publishing `Vanished(forgotten)`** (rev.5's contradictory
  wording fixed).
- Serialization: a terminal-intent atomic flag published FIRST, checked by the keeper callback before
  scheduling a remount and by the remount loop at every step boundary (bounds any waiter to one step + one
  backend timeout). A post-trip successful renewal is harmless (`tripMountLost` latched,
  `CasMountRuntime.cpp:68`).
- An in-flight GC round performs a **bounded abort** (may throw its `gc/state` corruption guard
  `CasGc.cpp:2427` or recreate control debris `:2436` — caught as a failed round); no new round after the
  flag.
- The disk stays registered; introspection distinguishes `identity_lost` / `vanished(erased|replaced|
  forgotten, since)` via the non-`store()`-gated snapshot (the system table must synthesize the lifecycle
  row instead of skipping the disk, `StorageSystemContentAddressedMounts.cpp:101`).

## 4. Blast radius

Transient: honest failures, auto-drain; fence-out recovery ≈ 36.5 s (TTL + 5 % + 5 s remount poll,
`CasServerRoot.cpp:390`, `CasPool.cpp:632`) + 30 s materialization grace if a ref lane was unsettled
(`CasPool.cpp:678`); the first renewal failure may precede fence-out by the remaining lease budget.
**`IdentityLost` [C5]: one such disk stalls ALL eligible Atomic `DROP` finalizations** (the finalizer
sweeps every registered disk under default `search_orphaned_parts_disks=ANY`,
`DatabaseCatalog.cpp:1657`) and any other all-disk operation that probes it, until `FORGET`/restart —
accepted: its origin is an out-of-contract erase or a genuinely broken backing, both operator-attention
events; the cure is one explicit verb.

| Caller | transient / `IdentityLost` | `Vanished` |
|---|---|---|
| `DROP` finalizer all-disk sweep (`DatabaseCatalog.cpp:1657`) | per-table throw → re-queue (existing catch `:1562`) → drains on recovery (transient) or after `FORGET` (`IdentityLost`); logs ~1/5 s per queued table (`ServerSettings.cpp:456`) — throttled | exists=false → skip (truth) |
| `table->drop()` of a table on the disk | throws → that table's `DROP` re-queues | removes no-op → **`DROP` completes** (txn constructible per the Factory class) |
| Orphan store-dir sweep (`DatabaseCatalog.cpp:1986`) | already never visits CA disks (`supportsStat`/`supportsChmod` false) — unaffected | same |
| `SYSTEM UNFREEZE` (`Freeze.cpp:147`), `system.remote_data_paths` (`:281`), `ATTACH AS REPLICATED` cleanup (`InterpreterCreateQuery.cpp:2807`), non-Atomic `DROP` (`DatabaseOnDisk.cpp:394`) | honest error; retry later | truthful absent/empty → proceed |
| MergeTree foreign-part scan (`MergeTreeData.cpp:2362/2398`; default `search_orphaned_parts_disks=ANY`, `MergeTreeSettings.cpp:2228`) | can fail an **unrelated non-default-policy** table load, and AsyncLoader does **not** retry (`AsyncLoader.cpp:468`) — permanent until manual `ATTACH`/restart. **Accepted + documented**; cure: `ATTACH` after recovery; guidance: `search_orphaned_parts_disks = LOCAL` where CA disks are configured. Inline/custom disks and default-policy tables unaffected | truthful absent → proceeds |
| `SELECT` on a loaded table of the disk | throw 668 | typed erased error — never silent-empty |
| `BACKUP` of such a table | throw | typed erased error (no silent empty backup) |

## 5. `SYSTEM CONTENT ADDRESSED FORGET '<disk>'`

As rev.5, with the join clarification of §3: (1) publish terminal-intent; (2) trip the fence; (3) stop
keeper renewal **without** a clean terminal release unless ref lanes provably drained (never an unearned
clean farewell); (4) stop GC (clear `i_am_leader`); (5) join keeper/remount/GC outside `remount_mutex`;
(6) set `Vanished(forgotten)`. Hazards documented: operator-wrong = the benign lie by human assertion +
no-op removes can orphan durable refs that a restart re-exposes; node-local. Own `AccessType`.

Not doing: runtime `STOP/START` (unsound without a process-wide disk-use barrier — rev.1 P0 #3), reuse
`UNMOUNT/MOUNT`, registry eviction, `UNMOUNT ALL`, and **mounting an inline `disk(...)` without a table**
(an early brainstorm goal — consciously dropped with the reuse lifecycle; admin verbs target registered
disks, and a config-defined disk covers the table-less-FSCK case).

## 6. `GC STOP '<disk>'` / `GC START '<disk>'`

As rev.5 (confirmed sound): scheduler only; `stop` clears `i_am_leader`; restartable; serialized under §3.

## 7. Introspection, FSCK

As rev.5 (confirmed sound): non-gated lifecycle snapshot; FSCK on a running disk — missing-manifest hard
*with* ref+HEAD revalidation; **`meta_without_body` advisory indefinitely** (GC suppresses meta-delete
errors, `CasGc.cpp:198`; no finite horizon exists); consumers to update: `FsckReport::clean`
(`CasFsck.h:113`), `gtest_cas_fsck.cpp:217`.

## 8. Known limits

Cross-session: a restart over a **fully** erased root re-bootstraps empty **only after the [C4] empty-prefix
proof**; over a partial erase it now fails loud (improvement over rev.5). Partial tampering with
`_pool_meta` intact: FSCK's domain. Erase-and-recreate under live processes: out of contract.
Pre-detection window (~10 s) after a live erase: mitigated by the empty-proof rule; writes can recreate
keys — covered by the contract. Non-strong-LIST backends: no natural `Vanished(erased)`; `FORGET` or
restart-after-full-deletion. Permanently-unconfirmable disk: stalls until `FORGET`/restart.

## 9. Rollback and tests

**Rollback of the landed Task 4–8 (the 2026-07-21 Dormant/UNMOUNT lifecycle) — the explicit list:**

1. **`MountState` enum** (`Mounted`/`Unmounting`/`Dormant`) and every branch on it → replaced by §1's
   storage-level lifecycle (`Constructing/Started/ShutDown`) + the pool condition
   (live / transient / `IdentityLost` / `Vanished`).
2. **`SYSTEM CONTENT ADDRESSED UNMOUNT` / `MOUNT` verbs** — parser + AST formatting cases,
   `ASTSystemQuery` fields, interpreter execution, and their **two `AccessType`s** (the `01271` grants
   reference loses those rows and gains `FORGET`'s).
3. **`unmountSynchronously`** + the `Unmounting` drain loop, and **`mountExplicitly`**.
4. **Part 6 benign-absent probes** — the `isMounted()`-guarded absent/empty answers
   (`ContentAddressedMetadataStorage.cpp:~557-564`, `:1114` existsDirectory, `:1370`
   iterateDirectory-empty, and siblings) → subsumed by the §1 central operation-class gate: transient /
   `IdentityLost` ⇒ throw; only proven/asserted `Vanished` answers truth-absent. No benign branch keyed on
   "not mounted" survives anywhere.
5. **FSCK-dormant-only** (Task 7 requirement) → FSCK on a running disk (§7).
6. **The four lifecycle gtests** asserting Dormant/UNMOUNT behavior (`gtest_ca_transaction.cpp:725` ff.) →
   replaced by the §9 test list below.
7. **The `GC RUN → UNMOUNT → FSCK → rm -rf` teardown pattern** in `04290`/`04295`/`05020` → replaced by
   the fail-closed `DROP SYNC → FORGET → verify → rm -rf` teardown below.

**Keep untouched:** Part 1 (below), the `poolAccess` coherent snapshot, atomic-publish `startup()`, the
`ca-fsck` rename, the GC-round entry-point gating (now via §3's lifecycle protocol — the raw entry points
`runGarbageCollectionRoundNow`/`runOneGcRoundForTest` get the same gate), and the **`GC RUN` `pending_*`
drain/result columns** (landed with the old lifecycle but independently useful — "rollback Task 4–8" must
not be misread as removing them).

**Part 1 — the landed abort-hardening contract** (transferred from the deleted 2026-07-21 spec; the code
is landed and stays):
- **Renewal path** (`MountLeaseKeeper::onRenewMismatch`, `CasServerRoot.cpp`): a re-read finding the mount
  key **absent** (backing deleted under a live mount) is an ENVIRONMENTAL condition, not a logic error —
  stop renewing fail-closed (fence latches to lost, never re-mint) via `FILE_DOESNT_EXIST`, **never**
  `LOGICAL_ERROR` (which aborts debug/ASan builds at exception *construction* under
  `abort_on_logical_error`). Genuine present-body foreign/superseded cases stay fatal.
- **Terminate path**: absent-lease during the clean-release terminal write is likewise environmental —
  complete the terminate as a no-op release (the object we would delete is already gone) or
  `FILE_DOESNT_EXIST` where the caller's contract requires a throw.
- **`CasMountLeaseLost` ProfileEvent**: counts terminal mount-lease losses (`vanished`, `superseded`,
  `foreign_writer`; NOT the recoverable `fenced_by_gc` branch), paired with a
  `system.content_addressed_log` `MountConflict` row.

**Tests — the stateless suite changes:** unique names+paths with invocation-random suffix, normalized
reference output (05020's reference prints a literal disk name today), **fail-closed teardown**
(`DROP SYNC` → `FORGET` → verify `vanished(forgotten)` in the system table → only then `rm -rf`; failed
`FORGET` aborts the test with `rm -rf` unreachable). Never `rm -rf` a globally-configured disk's data.

**The acceptance matrix (baseline + rev.6/7 additions):**
- **Transient**: lease key removed/restored (or backing blocked/unblocked) ⇒ access throws in the gap,
  auto-recovers after; a `DROP` issued in the gap re-queues and drains after recovery.
- **`Vanished` caller behavior**: on a vanished disk — that table's `DROP` completes (removes no-op, txn
  constructible); an unrelated table's `DROP` completes (truth-skip); `SELECT`/`BACKUP` fail with the typed
  erased error, never silent-empty; one WARN then log silence (G2).
- **Read-only empty-proof**: read-only pool over an erased backing ⇒ ATTACH fails typed, never attaches
  empty.
- **`FORGET` semantics**: on a live disk — fence tripped, keeper/remount/GC stopped and joined, no unearned
  clean farewell without a real drain, `vanished(forgotten)` visible in introspection.
- **`GC STOP/START`**: rounds stop (observable: no new rounds) and resume; reads/writes unaffected;
  `i_am_leader` cleared on stop.
- **`IdentityLost` observer** (rev.6): progressive erase — sentinels first ⇒ `IdentityLost` fail-loud, no
  benign answers, no auto-recovery; deletion completes ⇒ observer promotes to `Vanished(erased)` (gated on
  a strong-LIST backend: gtest/integration with the S3/rustfs harness or a mocked capability).
- **No-auto-revival** (rev.7): sentinels restored with matching identity while `IdentityLost` ⇒ state stays
  fail-loud; restart recovers.
- **Startup-over-partial-erase** (rev.6): missing meta + non-empty prefix ⇒ typed startup failure, zero
  non-probe writes.
- **Fence-generation** (rev.6): staging finalize / plain-object CAS racing a fence trip ⇒ typed abort, no
  durable write.

## 10. Review ledger

- rev.1 (codex): 3 P0 → identity gate; fresh-incarnation recovery kept; STOP dropped.
- rev.2: caller-side typed-catch — owner-rejected; eject — died on formulation.
- rev.3 (codex): **architecture endorsed**; amendments A1–A7.
- rev.4: A1–A7 folded; verification found fold blockers in A2/A3/A4 → 
- rev.5: B1–B6 (IdentityLost/Vanished split; admission semantics; empty-proof rescope; Factory class;
  ProbeResult boundary; identity fields). Verification: **B3/B4/B6/FSCK/teardown/FORGET-deadlock sound**;
  five narrow amendments →
- rev.6: [C1] non-absorbing `IdentityLost` + observer + one-way promotion; [C2] fence-generation checks
  on all durable-effect paths + proof window post-settle/grace; [C3] strong-LIST capability gates natural
  `Vanished(erased)`; [C4] empty-prefix precondition for missing-meta bootstrap; [C5] honest blast-radius
  wording; companion C2/C3 refined; consolidation (lost-content restoration + deletion of the three
  superseded spec files). Closing verification: **C3/C5 faithful, consolidation clean, Local walk-through
  passed**; four final amendments →
- rev.7: this document — [D1] full durable-lane quiescence before the erasure proof
  (outstanding-durable-request counter + GC exit + max-timeout grace); [D2] bootstrap ordered against the
  `_probe` battery + exclusive `pool_prefix` ownership; [D3] no auto-revival of `IdentityLost` on sentinel
  reappearance (restart recovers); [D4] companion R4/§8 dispositions, baseline acceptance matrix restored,
  `pending_*` columns in the keep-list.
