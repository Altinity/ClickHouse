# CAS disk lifecycle rev.6: throw-when-uncertain, truth-when-proven

Status: **DESIGN rev.6 — for final verification.** Rewrites rev.5 in place (git history holds rev.1–rev.5).

**rev.5 → rev.6 (per the codex rev.5 verification — 6 of 9 fold items were confirmed sound: B3, B4, B6,
FSCK-advisory, fail-closed teardown, the FORGET deadlock fix):** five narrow amendments **[C1]–[C5]** plus
cross-reference cleanups. The architecture ("uncertainty throws; genuinely proven old-generation loss
permits truthful absence") has survived three adversarial reviews unchanged.

- **[C1]** `IdentityLost` is fail-loud but **observationally non-absorbing**: a progressive `rm -rf`
  deletes the sentinels first — terminal absorption would strand the disk before the emptiness proof could
  complete. A low-rate, non-mutating lifecycle observer remains; one-way `IdentityLost → Vanished(erased)`.
- **[C2]** The "all commits are fence-gated" safety claim was **false** (`CasPlainObjects::casPutObject`
  writes table-level verbatim/dedup/rename objects with no `mayMutate` check). Every durable-effect path
  gets a fence-generation check; the emptiness-proof window opens only after ref lanes settle or the
  materialization grace elapses.
- **[C3]** Natural `Vanished(erased)` requires a **strong-LIST backend capability** (AWS S3, GCS —
  documented; RustFS pending evidence; Local/Emulated/unqualified gateways — never): elsewhere the only
  path to benign truth is `FORGET`.
- **[C4]** Startup bootstrap of a **missing `_pool_meta` requires a provably empty prefix**; a non-empty
  prefix without meta fails startup with zero writes (closes the restart-poisons-partial-pool hole and
  hardens the cross-session limit).
- **[C5]** Honest wording: one `IdentityLost` disk stalls **all** eligible Atomic `DROP` finalizations
  (default `search_orphaned_parts_disks=ANY` sweeps every disk) until `FORGET`/restart — accepted and
  stated, not narrowed to "drops involving the disk".

**Part 6 vs the terminal states (unchanged):** Part 6 answered absent when it *didn't know* (a lie).
`Vanished` answers absent when it is *proven or operator-asserted* (the truth). Everything in between —
`transient`, `IdentityLost` — **throws**.

Supersedes: rev.1–rev.5, the Path-B spec, the automount/eject spec, Parts 2–4 of the 2026-07-21 spec
(landed Task 4–8; **Part 1 KEPT**). Companion: `2026-07-22-cas-disk-lifecycle-problem-and-constraints.md`
(C2/C3 refined in step with this revision).

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
| Content read | real | **throw** 668 | **throw** typed "data root erased/decommissioned" |
| Create / write / rename (incl. previously-no-op sites; empty-txn commit) | real | **throw** 668 | **throw** typed erased |
| Remove | real | **throw** 668 | **no-op success** (truth) — a vanished-disk table's `DROP` completes |
| Admin (`store()`, GC RUN) | real | throw | throw typed erased (per-disk status in fan-outs) |

**Gate lifetime [C2]:** the gate governs **admission**, and additionally **every durable-effect
finalization is fence-generation-checked**: the plain-object surface (`CasPlainObjects::casPutObject` /
delete — table-level verbatim files, dedup metadata, rename copy/remove,
`ContentAddressedTransaction.cpp:748/:1312`) and staging-buffer finalize acquire a fence-generation token
at admission and re-check it immediately before the durable CAS/PUT; a mismatch aborts the write with the
typed transient error. The ref-log publish chokepoint is `appendRefOps` + the controlled staging methods
(not `mutateShard` — rev.5's wording corrected), already fence-coupled. Admitted, non-durable in-flight
**reads** may complete on their existing pipelines (their failure modes are the backend's own errors) —
this is the C3 refinement in the companion.

**The empty-proof rule** (unchanged from rev.5, confirmed sound): pre-terminal and on read-only pools, an
enumeration about to answer empty at a data root (`TableDir`, `DetachedContainer` `DirShape`s,
`ContentAddressedMetadataStorage.cpp:1303`) first confirms `_pool_meta` with an authoritative, uncached
probe; absent ⇒ typed throw. Terminal `Vanished` answers truth-empty directly. Enumeration latency now
couples to one backend GET on the empty path — the correct fail-closed trade-off, bounded by ordinary
request timeouts.

## 2. Typed erasure evidence

Single choke point: step 0 of `tryRemountOnce` (existing fresh-incarnation recovery kept verbatim;
`claimOwnerOrThrow`'s empty-root bootstrap unreachable from recovery).

`ProbeResult ∈ {Present(meta), KeyAbsent, ContainerAbsent, AccessDenied, Indeterminate}` implemented at the
`IObjectStorage`/raw-error boundary (raw HEAD error preserved before `getObjectInfo.cpp:135` discards it;
Local: explicit stat of the configured container/parent; container proof = `ListObjectsV2(max-keys=1,
prefix=pool_root)`; the IAM permutation table from the rev.4 review: only LIST-allowed+GET-allowed yields a
trustworthy 404, everything else stays transient).

- **`Present` + identity match** (`pool_id` + `blob_header_len`; format gate = successful compatible
  decode) → existing recovery. **`Present` + foreign `pool_id`** → `Vanished(replaced)`.
- **Sentinels absent, prefix non-empty** → **`IdentityLost`**: keeper/GC/writers stopped, **but the remount
  thread demotes to a lifecycle observer [C1]** — non-mutating, low-rate (renewal-period cadence with
  backoff), running only this typed probe; it never claims, allocates, or writes. One-way transition to
  `Vanished(erased)` when the proof below completes; `FORGET` and restart remain available. (This is how a
  progressive `rm -rf` — sentinels deleted first, the rest still draining — ends in `Vanished(erased)`
  instead of being stranded.)
- **`Vanished(erased)` proof [C2][C3]:** requires ALL of —
  1. the backend declares the **strong-prefix-LIST capability** [C3]: documented strongly-consistent LIST
     (AWS S3, GCS). RustFS: off until explicit evidence or an integration contract-test; Local/Emulated:
     **never** (Local listing is best-effort `LocalObjectStorage.cpp:450`, and an unmounted NFS/removable
     mount makes intact data look erased; Emulated scans before locking `CasObjectStorageBackend.cpp:1024`).
     Without the capability, `IdentityLost` is the terminal natural state and `FORGET` is the only path to
     benign truth — which is exactly the stateless-test story anyway (their teardown FORGETs).
  2. the window opens only after this pool's ref lanes settled or the full materialization grace (default
     30 s) elapsed since the fence trip [C2] — a durably-landing late ref PUT must not race the proof;
  3. container proof succeeds AND the full-prefix LIST returns **zero objects**, in ≥2 cycles spaced ≥ the
     renewal period; any error or non-empty result resets the counter.
- **Startup [C4]:** `PoolMeta::createOrValidate` may **create** a missing `_pool_meta` only after proving
  the pool prefix empty (same full-prefix LIST; on non-strong-LIST backends a single authoritative check —
  best effort against the weaker guarantee, still fail-closed on any object found). Missing meta over a
  non-empty prefix ⇒ startup fails with a typed error and **zero writes**. Restart is thereby a safe cure
  only after deletion completed; it can no longer mint a fresh identity over residual data
  (`CasPool.cpp:263/:310`).

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

Transient: honest failures, auto-drain; fence-out recovery ≈ 36.5 s (+30 s grace if a ref lane was
unsettled); the first renewal failure may precede fence-out by the remaining lease budget.
**`IdentityLost` [C5]: one such disk stalls ALL eligible Atomic `DROP` finalizations** (the finalizer
sweeps every registered disk under default `search_orphaned_parts_disks=ANY`,
`DatabaseCatalog.cpp:1657`) and any other all-disk operation that probes it, until `FORGET`/restart —
accepted: its origin is an out-of-contract erase or a genuinely broken backing, both operator-attention
events; the cure is one explicit verb. Other rows as rev.5: `Vanished` rows all proceed on truth;
transient/`IdentityLost` rows fail honestly; the `search_orphaned_parts_disks=ANY` + AsyncLoader-no-retry
table-load exposure stands accepted with `LOCAL` guidance; `SELECT`/`BACKUP` on a vanished disk get typed
errors, never silent-empty.

## 5. `SYSTEM CONTENT ADDRESSED FORGET '<disk>'`

As rev.5, with the join clarification of §3: (1) publish terminal-intent; (2) trip the fence; (3) stop
keeper renewal **without** a clean terminal release unless ref lanes provably drained (never an unearned
clean farewell); (4) stop GC (clear `i_am_leader`); (5) join keeper/remount/GC outside `remount_mutex`;
(6) set `Vanished(forgotten)`. Hazards documented: operator-wrong = the benign lie by human assertion +
no-op removes can orphan durable refs that a restart re-exposes; node-local. Own `AccessType`.

Not doing: runtime `STOP/START`, reuse `UNMOUNT/MOUNT`, registry eviction.

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

**Keep untouched:** Part 1 (abort-hardening: vanished/absent lease → `FILE_DOESNT_EXIST`/no-op, never
`LOGICAL_ERROR`), the `poolAccess` coherent snapshot, atomic-publish `startup()`, the `ca-fsck` rename, and
the GC-round entry-point gating (now via §3's lifecycle protocol — the raw entry points
`runGarbageCollectionRoundNow`/`runOneGcRoundForTest` get the same gate).

Tests — unique
names+paths with invocation-random suffix, normalized reference output, **fail-closed teardown**
(`DROP SYNC` → `FORGET` → verify `vanished(forgotten)` in the system table → only then `rm -rf`; failed
`FORGET` aborts the test with `rm -rf` unreachable) — plus rev.6 additions: `IdentityLost` observer test
(progressive erase: sentinels first ⇒ `IdentityLost` fail-loud; deletion completes ⇒ observer promotes to
`Vanished(erased)` — gated on a strong-LIST backend, gtest/integration with the S3/rustfs harness or a
mocked capability); startup-over-partial-erase test (missing meta + non-empty prefix ⇒ typed startup
failure, zero writes); fence-generation test (staging finalize / plain-object CAS racing a fence trip ⇒
typed abort, no durable write).

## 10. Review ledger

- rev.1 (codex): 3 P0 → identity gate; fresh-incarnation recovery kept; STOP dropped.
- rev.2: caller-side typed-catch — owner-rejected; eject — died on formulation.
- rev.3 (codex): **architecture endorsed**; amendments A1–A7.
- rev.4: A1–A7 folded; verification found fold blockers in A2/A3/A4 → 
- rev.5: B1–B6 (IdentityLost/Vanished split; admission semantics; empty-proof rescope; Factory class;
  ProbeResult boundary; identity fields). Verification: **B3/B4/B6/FSCK/teardown/FORGET-deadlock sound**;
  five narrow amendments →
- rev.6: this document — [C1] non-absorbing `IdentityLost` + observer + one-way promotion; [C2]
  fence-generation checks on all durable-effect paths (`CasPlainObjects`, staging finalize) + proof window
  opens post-settle/grace; [C3] strong-LIST backend capability gates natural `Vanished(erased)` (S3/GCS
  yes; RustFS pending; Local/Emulated never — `FORGET` is their path); [C4] empty-prefix precondition for
  missing-`_pool_meta` bootstrap at startup; [C5] honest `IdentityLost` blast-radius wording; companion
  C2/C3 refined; `appendRefOps` chokepoint and FORGET-join wording corrected.
