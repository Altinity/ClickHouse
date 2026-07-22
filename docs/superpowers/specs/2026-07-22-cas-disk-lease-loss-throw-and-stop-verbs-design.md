# CAS disk lifecycle rev.5: throw-when-uncertain, truth-when-proven

Status: **DESIGN rev.5 — for final verification.** Rewrites rev.4 in place (git history holds rev.1–rev.4).

**rev.4 → rev.5 (per the codex rev.4 verification):** the rev.3→rev.4 fold introduced three new blockers of
my own making; rev.5 fixes them as **[B1]–[B6]**. The architecture ("uncertainty throws; genuinely proven
old-generation loss permits truthful absence") has now survived two adversarial reviews unchanged.

**The biggest conceptual change [B1]:** natural detection alone **never** grants benign-absent. Losing the
identity sentinels proves only *identity loss*, not *erasure* (a partial bulk delete can remove exactly
`_pool_meta` + the owner key while refs/manifests/blobs survive — benign answers would then lie about live
data, violating C2). So the terminal states split:

- **`IdentityLost`** — identity sentinels gone but erasure NOT proven: terminal for background work (threads
  stop, one WARN), **fail-loud for every operation**. Cure: `FORGET` or restart.
- **`Vanished`** — benign truth (absent/empty/no-op-removes + typed loud errors for content reads/writes),
  reachable ONLY by: (a) **pool-wide emptiness proof** — container reachable AND a full-prefix
  `ListObjectsV2(pool_root, max-keys=1)` returns **zero objects**, in ≥2 cycles spaced ≥ the renewal period
  (`rm -rf` of the pool produces exactly this; partial deletion leaves objects → `IdentityLost`); or
  (b) `_pool_meta` present with a different `pool_id` → `Vanished(replaced)` (authoritative proof our
  generation is gone; removes are no-op so the foreign pool's objects are never touched); or (c) explicit
  operator **`FORGET`** → `Vanished(forgotten)`.

This is *stricter* than every earlier revision: the disk lies to no one, ever — benign answers require
either a proof that the whole prefix is empty, or a human's explicit destructive assertion.

**Part 6 vs `Vanished` (unchanged):** Part 6 answered absent when it *didn't know* (a lie); `Vanished`
answers absent when it is *proven* (the truth); everything in between (`transient`, `IdentityLost`) throws.

Supersedes: rev.1–rev.4, the Path-B spec, the automount/eject spec, Parts 2–4 of the 2026-07-21 spec
(landed Task 4–8; **Part 1 KEPT**). Companion: `2026-07-22-cas-disk-lifecycle-problem-and-constraints.md`.

---

## 1. States and the operation-class gate [B3][B4]

```
[Running/live] ──lease lost──► [transient-not-live] ──┬─ identity sentinels gone, prefix NOT empty ──► [IdentityLost]  (terminal, fail-loud)
      ▲                            │ (all access throws;   ├─ pool-wide emptiness proof ×2 ──────────────► [Vanished(erased)]    (terminal, truth)
      └── self-remount succeeds ───┘  remount retries)     └─ _pool_meta present, foreign pool_id ──────► [Vanished(replaced)]  (terminal, truth)
                                                     FORGET (any live/not-live state) ─────────────────► [Vanished(forgotten)] (terminal, truth)
```

Plus the minimal **storage-level** lifecycle `Constructing → Started → ShutDown` (codex rev.3 #9): outside
`Started`, the existing null-pool check in the `poolAccess` coherent snapshot fails loud. Two residual
states, not a reuse machine.

**Operation classes — SIX [B4]**, enforced centrally at every public metadata/transaction entry (not by
re-keying the seven Part-6 branches; the known short-circuit offenders — `liveTreeDirHasChildren`
hardcoded-true `ContentAddressedMetadataStorage.cpp:883`, `isDirectoryEmpty` part-dir short-circuit `:1385`,
no-op `createDirectory*` `ContentAddressedTransaction.cpp:924`, empty-txn commit `:408`,
`setLastModified`/`setReadOnly` `:1109`, and the `tryGetInManifestBytes` catch-all `:1488` which must be
narrowed to not convert typed throws into `FILE_DOESNT_EXIST` — are all routed through the gate). The
implementation plan must publish the **complete method→class inventory** (codex rev.4 J).

| Class | live | transient / `IdentityLost` | `Vanished` |
|---|---|---|---|
| **Factory / capability / introspection** (`createTransaction`, `getType`, `getPath`, capability getters, lifecycle snapshot, `gcHealth`) | works | **works** (no backing I/O) — a `DROP`'s removal transaction must be constructible on a vanished disk (`DiskObjectStorageTransaction.cpp:73` constructs eagerly) | **works** |
| Probe / enumeration | real | **throw** 668 | absent / empty (truth) |
| Content read | real | **throw** 668 | **throw** typed "data root erased/decommissioned — recreate the disk; restart re-registers the name" |
| Create / write / rename (incl. previously-no-op `createDirectory*`, `setLastModified`; empty-txn commit) | real (no-ops stay no-ops when live) | **throw** 668 | **throw** typed erased |
| Remove | real | **throw** 668 | **no-op success** (truth) — `DROP` of a vanished-disk table completes |
| Admin (`store()`, GC RUN) | real | throw | throw typed erased (per-disk status in fan-outs) |

**Gate lifetime = admission semantics [B2]** (explicit weakening of C3, with the safety argument):
already-issued I/O pipelines bypass the metadata storage (`DiskObjectStorage::prepareRead` resolves once,
`DiskObjectStorage.cpp:813/:879`; S3-staging opens an object-storage sink directly,
`ContentAddressedTransaction.cpp:824`). The gate governs **operation admission**, not in-flight streams.
Safety: (a) all *commits* (ref-log publish, txn finalize) ARE fence-gated (`mutateShard`), so a staging PUT
landing after fence loss is unreferenced debris reclaimed by GC — never visible data; (b) in-flight reads on
an actually-erased backing fail naturally with backend errors; on a fenced-but-intact backing they briefly
read intact data — same as today, within RMT interserver-trust semantics. Optional hardening (plan item):
a fence-generation token recheck at staging-buffer finalize.

**The empty-proof rule, rescoped [B3]:** *pre-terminal only* (live and transient states) and on **read-only
pools** (which have no keeper and hence no natural detection — this rule is their only line of defense,
fixing the read-only ATTACH silent-empty): an enumeration about to answer **empty at a data root** (the
qualifying `DirShape`s: `TableDir`, `DetachedContainer` — the routing already exists,
`ContentAddressedMetadataStorage.cpp:1303`) must first confirm `_pool_meta` presence with an
**authoritative, uncached** probe; absent ⇒ throw typed. **A cached positive never authorizes an empty
data-root answer** (rev.4's short-TTL cache is dropped — it left a silent-empty window). In terminal
`Vanished` the rule does not apply — enumeration answers the promised truth-empty directly (rev.4's
contradiction removed). Cost: one GET per empty-data-root enumeration on load/attach paths — acceptable.
Partial namespace erasure with `_pool_meta` intact remains FSCK's domain (known limit).

## 2. Typed erasure evidence [B5]

Single choke point: step 0 of `tryRemountOnce` (`CasPool.cpp:640-725`); the existing fresh-incarnation
recovery is kept verbatim; `claimOwnerOrThrow`'s empty-root bootstrap is unreachable from recovery.

**`ProbeResult ∈ {Present(meta), KeyAbsent, ContainerAbsent, AccessDenied, Indeterminate}`, implemented at
the `IObjectStorage`/raw-error boundary** — the flattening happens *below* today's `Backend`
(`CasBackend.h:183` `optional`; S3 merges `NO_SUCH_KEY`/`NO_SUCH_BUCKET`/`RESOURCE_NOT_FOUND` at
`getObjectInfo.cpp:90/:135` → `S3ObjectStorage.cpp:609` `nullopt`; Local maps missing-parent to `nullopt`,
`LocalObjectStorage.cpp:397`), so the raw HEAD error must be preserved before `getObjectInfoIfExists`
discards it. Local/Emulated: an explicit stat of the configured container (or parent) distinguishes
deleted-prefix from missing-key.

- **Container proof** = `ListObjectsV2(max-keys=1, prefix=pool_root)` (not bucket HEAD). IAM permutations
  (verified against AWS semantics): `ListBucket(prefix)`+`GetObject` allowed → absence yields a trustworthy
  404; LIST allowed + GET denied → key probe is 403 → stay transient; LIST denied → container proof fails →
  stay transient; LIST denied + key absent → AWS answers 403 → stay transient. A gateway that masks denied
  GET as 404 is indistinguishable — excluded by contract (capability-probe territory).
- **`Vanished(erased)`** = container proof succeeds AND the full-prefix LIST is **empty** (zero objects
  under `pool_root` — the pool-wide emptiness proof of [B1], which subsumes per-key sentinels), in ≥2 cycles
  spaced ≥ the renewal period; any error or non-empty result resets the counter. Identity sentinels absent
  but prefix non-empty → `IdentityLost` (fail-loud).
- **Identity comparison** on `Present`: **`pool_id` + `blob_header_len` only** [B6] — both proven immutable
  (`pool_id` minted once, preserved by admission CAS, `CasPoolMeta.cpp:105`; `blob_header_len`
  persisted-authoritative, `:117`). "Format identity" = successful compatible decode, not header equality
  (every legal admission rewrites the object with the current writer header, `CasPoolMetaFormat.cpp:70`).
  `algos_used`/`min_reader_generation` legally mutate — refreshed, never compared.
- Versioned buckets: moot for writable pools — the capability probe already rejects delete-marker-producing
  versioned buckets (`CasProbe.cpp:210`); rev.4's delete-marker rule is dropped as unreachable.

**[A1, kept] Erase-and-recreate at the same prefix while any old process may live is OUT OF CONTRACT**
(keys are not `pool_id`-namespaced; a still-open fence has up to ~TTL of write authority). **`FORGET` is
node-local** (the owner/mount protocol is per `server_root_id`, `CasLayout.h:295`) — it is *not*
authorization to erase or reuse shared backing; same-prefix reuse requires stopping/fencing **every**
member (ideally `ON CLUSTER`) first.

## 3. Entering a terminal state; lifecycle serialization [B4-races]

- One WARN + ProfileEvent (`CasDataRootVanished` / `CasIdentityLost`); the remount thread exits its own
  loop; the GC scheduler observes the flag at its next tick and exits; the keeper already exited. Joins only
  in `~Pool` (C6).
- **Serialization protocol** (natural transitions, `FORGET`, `GC STOP/START`, `tryRemountOnce` — one
  protocol): a **terminal-intent atomic flag is published FIRST**, before any lock, and is checked (i) by
  the keeper's callback before scheduling a remount, (ii) by the remount loop at each step boundary (so an
  in-flight `tryRemountOnce` — which may hold `remount_mutex` across its ~36.5 s observation loop +
  backend timeouts, `CasPool.cpp:621` — aborts at the next boundary, bounding any waiter's delay to one
  step + one backend timeout). **All thread joins happen outside `remount_mutex`** (the codex rev.4 wait
  cycle: FORGET holds mutex → keeper schedules remount → remount blocks on mutex → FORGET joins it →
  deadlock — is thereby impossible). A post-trip successful renewal is harmless: `tripMountLost` is latched;
  renew updates only the deadline, never clears `lost` (`CasMountRuntime.cpp:68`).
- An in-flight GC round during the transition performs a **bounded abort**: it may observe missing
  `gc/state` (throws its corruption guard, `CasGc.cpp:2427`) or recreate control debris (`:2436`) — caught
  by the scheduler as a failed round; no *new* round starts after the flag. Documented as bounded abort,
  not "harmless completion".
- The disk stays registered (no eject); introspection shows `identity_lost` / `vanished(reason, since)`
  distinctly.

## 4. Blast radius (with real numbers)

Transient: honest failures, auto-drain. Fence-out recovery window **36.5 s** (TTL+5 %+5 s poll) **+30 s**
grace if a ref lane was unsettled; the first renewal failure may precede fence-out by the remaining lease
budget (codex rev.4 I). `IdentityLost`: same fail-loud surface, but no auto-recovery — `DROP`s involving
the disk re-queue until `FORGET`/restart (out-of-contract territory by construction).

| Caller | transient / `IdentityLost` | `Vanished` |
|---|---|---|
| `DROP` finalizer sweep (`DatabaseCatalog.cpp:1657`) | per-table throw → re-queue (existing catch `:1562`) → drains on recovery (transient) or after `FORGET` (`IdentityLost`); log-throttled | exists=false → skip (truth) |
| `table->drop()` on the disk | throws → re-queues | removes no-op → **`DROP` completes** (txn constructible per the Factory class [B4]) |
| `SYSTEM UNFREEZE`, `system.remote_data_paths`, `ATTACH AS REPLICATED` cleanup, non-Atomic `DROP` | honest error | truthful absent/empty → proceed |
| MergeTree foreign-part scan (`search_orphaned_parts_disks` default `ANY`; AsyncLoader does not retry) | can permanently fail an unrelated non-default-policy table load until manual `ATTACH` — **accepted + documented**; guidance: `LOCAL` where CA disks are configured | truthful absent → proceeds |
| `SELECT` / `BACKUP` on a loaded table | throw 668 | typed erased error (never silent-empty) |

## 5. `SYSTEM CONTENT ADDRESSED FORGET '<disk>'`

Operator fire-marshal verb + the tests' teardown handle. Protocol (under §3's serialization):
1. Publish terminal-intent (blocks new remount scheduling).
2. Trip the local fence (`mayMutate` → false) — allowed on a live disk; the trip *is* the deliberate act.
3. Stop keeper background renewal **without** the clean terminal release unless ref lanes are provably
   drained (`~Pool`'s rule, `CasPool.cpp:565`); otherwise leave the lease to expire by observation — never
   an unearned clean farewell (`CasServerRoot.cpp:882`).
4. Stop the GC scheduler (clear `i_am_leader`); joins outside `remount_mutex`.
5. Set `Vanished(forgotten)`.

Hazards, documented loudly: operator-wrong ⇒ the benign lie by human assertion, **plus** no-op removes make
ClickHouse forget table/part state while durable refs remain (restart over the intact pool re-exposes
them); `FORGET` is **node-local** (§2). Own `AccessType` under `SYSTEM`.

Not doing: runtime `STOP/START`, reuse `UNMOUNT/MOUNT`, registry eviction.

## 6. `GC STOP '<disk>'` / `GC START '<disk>'`

As rev.4: GC scheduler only; `stop` clears `i_am_leader`; restartable (`gc_id` preserved); serialized under
§3. GC's `gc/state` lease is independent of the mount lease; its destructive protocol is CAS-protected and
stale-leader tolerant.

## 7. Introspection, FSCK

- Non-`store()`-gated lifecycle snapshot in `system.content_addressed_mounts`: name, lifecycle
  (`live`/`not_live`/`identity_lost`/`vanished(reason, since)`), last-known identity.
- FSCK on a running disk: dormant-only dropped. **Missing committed manifest** stays hard *with*
  revalidation (exact ref re-resolve + exact-object HEAD — the sound `Dangling` pattern, `CasFsck.cpp:370`).
  **`meta_without_body` is advisory indefinitely** [codex rev.4 #7]: GC's meta-delete jobs suppress errors
  (`CasGc.cpp:198`) and `wait` proves only completion (`:601`), so no finite grace horizon makes persistence
  hard evidence; a hard counter returns only if GC gains a durable retry/repair protocol. Consumers to
  update: `FsckReport::clean` (`CasFsck.h:113`) and the gtest asserting hard (`gtest_cas_fsck.cpp:217`).
  One-row summary unchanged (`meta_without_body` is not in the SQL row, `InterpreterSystemQuery.cpp:2402`).

## 8. Known limits (explicit non-goals)

Cross-session (restart over an erased root re-bootstraps empty — today's cold-open semantics; future: local
marker for a boot warning). Partial tampering with `_pool_meta` intact (FSCK's domain). Erase-and-recreate
under live processes (out of contract). Pre-detection window (~10 s) after an out-of-contract live erase —
mitigated by the empty-proof rule for enumeration loads; writes can recreate keys (S3 PUT creates) — covered
by the contract declaration. Permanently-unconfirmable disk: `DROP` delays until `FORGET`/restart.

## 9. Rollback and tests [B-tests]

**Rollback of landed Task 4–8**: `MountState` enum + branches → §1's storage lifecycle + pool condition;
`UNMOUNT`/`MOUNT` verbs + two `AccessType`s + AST fields + parser/formatter cases + `unmountSynchronously` +
`mountExplicitly` + drain loop; FSCK-dormant-only; the four lifecycle gtests (`gtest_ca_transaction.cpp:725`
ff.); the old teardown patterns in `04290`/`04295`/`05020`. Part-6 sites subsumed by the §1 central gate.
Keep: Part 1, `poolAccess` snapshot, atomic `startup()`, `ca-fsck` rename, GC entry-point gating (via §3;
the raw entry points `runGarbageCollectionRoundNow`/`runOneGcRoundForTest` get the same gate —
`store()` throwing does not cover them, `ContentAddressedMetadataStorage.cpp:407`).

**Tests**:
- Unique per-invocation disk **names and paths**: `${CLICKHOUSE_TEST_UNIQUE_NAME}` **plus an
  invocation-random suffix** (it does not vary under a fixed `--database` rerun, `shell_config.sh:13`);
  update SQL predicates; **normalize or omit the disk-name column in reference output** (05020's reference
  currently prints a literal name).
- **Teardown, fail-closed** [codex rev.4 #10]: `DROP TABLE ... SYNC` → `SYSTEM CONTENT ADDRESSED FORGET` →
  **verify** `system.content_addressed_mounts` reports `vanished(forgotten)` → only then `rm -rf`. A failed
  `FORGET` (permissions, timeout, wrong name) aborts the test loudly — `rm -rf` must be unreachable on the
  failure path (the scripts have no `set -e`; the check is explicit). Never `rm -rf` a globally-configured
  disk's data. Stateless-test privileges for the new `AccessType` must be verified (default user in tests is
  privileged; the grants test `01271` gets the new rows).
- New tests: `IdentityLost` (partial erase: sentinels gone, prefix non-empty ⇒ fail-loud, no benign answers,
  no auto-recovery); `Vanished(erased)` (full `rm -rf` ⇒ after proof, truth-empty; that table's `DROP`
  completes; unrelated `DROP` completes; one WARN then silence); transient (gap ⇒ throws, auto-recovers,
  `DROP` drains); empty-proof (read-only pool over erased backing ⇒ ATTACH fails typed, never empty);
  `FORGET` (live disk: fence tripped, no unearned clean farewell, `vanished(forgotten)` in introspection);
  `GC STOP/START`.

## 10. Review ledger

- rev.1 (codex): 3 P0 (resurrect-erased-pool; nonexistent recovery mechanism; unsound STOP) → identity
  gate; fresh-incarnation recovery kept; STOP dropped.
- rev.2: caller-side typed-catch — owner-rejected (Part-6 redux); eject — died on formulation.
- rev.3 (codex): **architecture endorsed**; protocol amendments A1–A7.
- rev.4: A1–A7 folded; codex verification: fold recognized, but **A2/A3/A4 introduced new blockers** →
- rev.5: this document — [B1] `IdentityLost`/`Vanished` split (benign only via pool-wide emptiness proof or
  `FORGET`); [B2] admission semantics for issued I/O with the fence-gated-commit safety argument; [B3]
  empty-proof pre-terminal + uncached authoritative positive + enumerated `DirShape`s; [B4] the sixth
  Factory/introspection class (un-blocks `DROP`'s transaction construction); [B5] `ProbeResult` at the
  `IObjectStorage`/raw-error boundary + `ListObjectsV2` container proof + IAM table + Local stat semantics;
  [B6] identity = `pool_id`+`blob_header_len`, format = compatible decode; FORGET ordering (terminal-intent
  first, joins outside `remount_mutex`, bounded-abort GC); `meta_without_body` advisory indefinitely;
  fail-closed test teardown.
