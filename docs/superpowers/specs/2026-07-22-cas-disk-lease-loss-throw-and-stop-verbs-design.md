# CAS disk lifecycle rev.4: throw-when-uncertain, truth-when-proven (`Vanished`)

Status: **DESIGN rev.4 — converged.** Rewrites rev.3 in place (git history holds rev.1–rev.3).

The codex rev.3 review **endorsed the architecture** — "the high-level policy is implementable: transient
uncertainty should fail loud, and a genuinely proven loss of the old generation may truthfully answer absent
while content operations throw" — and returned protocol-level findings. rev.4 folds in its "smallest set of
amendments" (items 1–7 of the review's overall judgment), mapped inline below as **[A1]…[A7]**.

**The core idea (unchanged since rev.3):** the correct not-live behavior depends **only on the disk's own
state**, never on the table–disk relationship. Transient-not-live ⇒ **throw** to everyone (a delay that
auto-drains, not a wedge). Positively-proven-erased (`Vanished`) ⇒ answer **the truth** (absent/empty/no-op
for removes; typed loud errors for content reads and writes). The own-vs-foreign discriminator, the
DROP-finalizer guard, `IStorage`→disks contracts, and registry eviction all dissolve. Generic/upstream code
changes: **zero**.

**Part 6 vs `Vanished` — the honesty distinction this design rests on:**

| | Part 6 (rolled back) | `Vanished` (rev.4) |
|---|---|---|
| Gate condition | "not Mounted" — **ambiguous**, data possibly intact | typed evidence **proved** the data root is erased/replaced |
| exists/list answers | absent/empty — **a lie** when data is intact | absent/empty — **the truth** |
| Content reads | absent-ish | **typed throw** ("data root erased") |
| Writes/creates | benign | **typed throw** |
| Transient outage | lied "absent" | **throws** (no benign answer while uncertain) |

This is constraint C2 / requirement R2 of the companion
`2026-07-22-cas-disk-lifecycle-problem-and-constraints.md`: benign-absent is legitimate **only** in a
positively-established data-erased state.

Supersedes: rev.1–rev.3 of this file, the Path-B self-quiesce spec, the automount/eject spec, and Parts 2–4
of `2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md` (landed Task 4–8; **Part 1 KEPT**).

---

## 1. State model and the operation-class gate [A4]

Per writable pool, one runtime condition (atomic, not persisted):

```
[Running/live] ──lease lost──► [transient-not-live] ──typed evidence proves erased/replaced──► [Vanished]
      ▲                              │  (self-remount retries; ALL access throws)               (terminal)
      └──── self-remount succeeds ───┘
```

Plus a **minimal storage-level lifecycle** that survives the `MountState` rollback (codex rev.3 #9): the
metadata storage itself is `Constructing → Started → ShutDown`; outside `Started` every access fails loud on
the null-pool check of the existing `poolAccess` coherent snapshot. This is two residual states on the
storage, not a reuse machine.

**One central gate, five operation classes, enforced at EVERY public metadata and transaction entry** — not
by re-keying the seven Part-6 branches (the rev.3 mistake; codex found short-circuit paths that never reach
`poolAccess`: `liveTreeDirHasChildren` hardcoded-true `ContentAddressedMetadataStorage.cpp:883`,
`isDirectoryEmpty` part-dir short-circuit `:1385`, no-op `createDirectory`/`createDirectoryRecursive`
`ContentAddressedTransaction.cpp:924`, empty-transaction commit `:408`, `setLastModified`/`setReadOnly`
`:1109`, and the `tryGetInManifestBytes` catch-all `:1488` that converts a typed throw into
`FILE_DOESNT_EXIST` — that catch must be narrowed):

| Class | Running/live | transient-not-live | `Vanished` |
|---|---|---|---|
| Probe / enumeration | real answer | **throw** 668 ("mount lease not held — backing may be temporarily unreachable") | absent / empty (truth), subject to the empty-proof rule below |
| Content read | real | **throw** 668 | **throw** typed "data root erased — recreate the disk; restart re-registers the name" |
| Create / write / rename (incl. the previously-no-op `createDirectory*`, `setLastModified`, empty-txn commit) | real (no-ops stay no-ops **when live**) | **throw** 668 | **throw** typed erased |
| Remove | real | **throw** 668 | **no-op success** (truth: nothing to remove) — lets `DROP` of a vanished-disk table complete |
| Admin (`store()`, GC RUN) | real | throw | throw typed erased (per-disk status in fan-outs, no silent no-op) |

**The empty-proof rule [A3]:** any enumeration about to answer **empty at a data root** (table dir /
namespace level) must first confirm `_pool_meta` presence (typed probe of §2, short-TTL cached). Absent ⇒
throw typed erased instead of answering empty. This applies to **writable and read-only pools alike** and
closes the two silent-empty-load holes codex found: read-only ATTACH (directory-create and `format_version`
write are *skipped* on read-only disks, `MergeTreeData.cpp:469/:501`, so enumeration is the only line of
defense there — read-only pools are hereby **in scope**, fixing rev.3's false exclusion) and the short
pre-detection window on a writable pool after an out-of-contract erase (§8).

- Gate signal: the pool's local atomic fence (`mayMutate` machinery, TTL hysteresis — a single slow renewal
  does not fence).
- Reads are gated in transient (a tightening vs today, where only mutations are fence-gated) — required by
  the mount contract (C3); the hysteresis keeps normal operation unaffected.

## 2. Typed erasure evidence (the identity gate) [A1][A2][A7]

Single choke point: **step 0 of `tryRemountOnce`** (`CasPool.cpp:640-725`; the existing fresh-incarnation
recovery — fresh writer epoch → `quiesceRefTablesForRemount` → `setLiveWriterEpoch` → `armMountFence` — is
kept verbatim, and `claimOwnerOrThrow`'s empty-root bootstrap becomes unreachable from recovery).

**A typed probe below `Backend`** replaces the current `optional`-flattening (`CasBackend.h:183` collapses
NoSuchKey / NoSuchBucket / generic RESOURCE_NOT_FOUND into one `nullopt`, via `getObjectInfo.cpp:90/:135`):

`ProbeResult ∈ { Present(meta), KeyAbsent, ContainerAbsent, AccessDenied, Indeterminate }`

Verdict rules:

1. **Present + immutable identity matches** → genuine fence-out/transient → proceed with the existing
   recovery. **[A7] Immutable comparison fields only**: `pool_id`, `blob_header_len`, format identity.
   `algos_used` and `min_reader_generation` **legally mutate** via CAS admission by other nodes
   (`CasPoolMeta.cpp:63`) — they are refreshed, never treated as replacement evidence.
2. **Present + immutable identity mismatch** → `Vanished(replaced)` immediately (authoritative proof of
   foreign content).
3. **`Vanished(erased)` requires, in each of ≥2 probe cycles spaced ≥ the mount renewal period apart** (not
   the 1-second remount backoff — two adjacent samples are debounce, not proof): container access
   **succeeds** (proves connectivity + auth) **AND** `_pool_meta` is `KeyAbsent` **AND** a second immutable
   sentinel (the owner-claim key) is also `KeyAbsent`. `ContainerAbsent`, `AccessDenied`, `Indeterminate`,
   or any error ⇒ reset the counter, stay transient, retry forever. **An erased verdict is never derived
   from errors or from a single evidence kind.**
4. Versioned buckets: a delete marker legitimately makes the current generation absent — treated as erased
   (the current generation *is* gone); documented.

**[A1] Erase-and-recreate under live processes is UNSUPPORTED.** Pool object keys are not namespaced by
`pool_id` (`CasLayout.cpp:10`; envelope deliberately carries no domain id), so a node whose local fence is
still open (up to TTL ≈ 30 s after the last confirmed renewal) can write stale-incarnation objects into a
pool recreated at the same prefix before detection fires. Namespacing every key by `pool_id` is
disproportionate; instead the contract is: **erasing a pool's backing and recreating a pool at the same
prefix while any process that mounted the old pool may still be running is out of contract.** The identity
gate is this node's lifecycle protection and a diagnostic — not a cross-node atomic fence. (Tests conform:
`DROP SYNC → FORGET → rm -rf`, §9, and never re-create at the same prefix.)

## 3. On entering `Vanished`

- Set the terminal flag; one WARN ("CAS data root for '<disk>' is erased/replaced — disk is vanished;
  reads/writes fail, removals no-op; recreate the pool and restart to reuse the name") +
  `CasDataRootVanished` ProfileEvent; the remount thread exits its own loop; the GC scheduler observes the
  flag at its next tick and exits; the keeper already exited. Joins happen only in `~Pool` (C6).
- **Lifecycle serialization [A5]**: the natural transition, `FORGET`, `GC STOP/START`, and `tryRemountOnce`
  all serialize under one lifecycle protocol (the remount mutex), and the raw GC entry points
  (`runGarbageCollectionRoundNow` / `runOneGcRoundForTest`, which read `cas_store` directly,
  `ContentAddressedMetadataStorage.cpp:407`) get the same gate — `store()` throwing does not cover them
  (codex rev.3 #9). An in-flight GC round completes against the erased backing harmlessly (404-tolerant
  fold) but no *new* round starts after the flag.
- The disk **stays registered** (no eject): sweeps get truthful answers, loaded tables get typed errors,
  introspection still shows it, and re-`CREATE` with the same name gets an honest throw, not a silent
  re-bootstrap. One quiesced husk per erased disk until restart — accepted.

## 4. Blast radius (deliberate, enumerated, with real numbers)

Transient-not-live: honest failures, like a dead NFS mount ("пусть ломает"). Measured window for a normal
fence-out recovery: **36.5 s** (TTL + 5 % + 5 s remount poll, `CasServerRoot.cpp:390`,
`CasPool.cpp:632`) **+ 30 s** materialization grace if a ref lane was unsettled (`CasPool.cpp:678`); a live
twin keeps the disk transient indefinitely (correct — it is not ours to serve).

| Caller | transient-not-live | `Vanished` |
|---|---|---|
| `DROP` finalizer sweep (`DatabaseCatalog.cpp:1657`) | per-table throw → re-queue (existing catch `:1562`) → auto-drains; logs ~1/5 s per queued table (`ServerSettings.cpp:456`) — add log throttling | exists=false → skip (truth) |
| `table->drop()` on the disk | throws → that table's `DROP` re-queues until recovery | removes no-op → `DROP` completes |
| `SYSTEM UNFREEZE`, `system.remote_data_paths`, `ATTACH AS REPLICATED` cleanup, non-Atomic `DROP` | honest error; retry later | truthful absent/empty → proceed |
| **MergeTree foreign-part scan** (`MergeTreeData.cpp:2362/2398`) | **`search_orphaned_parts_disks` defaults to `ANY`** (`MergeTreeSettings.cpp:2228`), so a transient *config-registered* CA disk fails loading an **unrelated non-default-policy** table, and **AsyncLoader does not retry** failed loads (`AsyncLoader.cpp:468`) — permanent until manual `ATTACH`/restart. **Accepted + documented** [owner decision]: cure is `ATTACH` after recovery; deployment guidance: `search_orphaned_parts_disks = LOCAL` where CA disks are configured. Inline/custom disks and default-policy tables are unaffected. | truthful absent → proceeds |
| `SELECT` on a loaded table | throw 668 | typed erased error (loud, never silent-empty) |
| `BACKUP` of such a table | throw | typed erased error |

## 5. `SYSTEM CONTENT ADDRESSED FORGET '<disk>'` [A5]

Operator fire-marshal verb and the tests' teardown handle. **Protocol** (serialized under the §3 lifecycle
protocol; codex rev.3 #6):

1. **Trip the local fence first** (`mayMutate` → false) — `FORGET` is allowed on a live disk; tripping the
   fence *is* the deliberate act.
2. Stop keeper background renewal **without the clean terminal release** unless ref lanes are provably
   drained (`Pool::~Pool`'s rule, `CasPool.cpp:565`) — never publish an unearned clean farewell
   (`min_active = UINT64_MAX` stamping, `CasServerRoot.cpp:882`, only after a real drain); otherwise leave
   the lease to expire by observation.
3. Stop the GC scheduler (clear `i_am_leader`).
4. Prevent a racing `tryRemountOnce` from installing a keeper (checked under the same serialization).
5. Set `Vanished(forgotten)`.

Operator-wrong hazard, documented loudly: if the data was intact, `FORGET` reintroduces the benign lie by
explicit human assertion (`umount -f`), **and worse** — no-op removes let ClickHouse forget table/part state
while durable refs remain, so a restart over the intact pool can re-expose refs or leave permanently
unreachable state. `FORGET` is for pools the operator asserts are gone. Own `AccessType` under `SYSTEM`.

Deliberately not doing: runtime `STOP/START` (unsound without a process-wide disk-use barrier), reuse
`UNMOUNT/MOUNT`, registry eviction.

## 6. `SYSTEM CONTENT ADDRESSED GC STOP '<disk>'` / `GC START '<disk>'`

As rev.3: only the GC scheduler; disk fully usable meanwhile; `stop` clears `i_am_leader`; restartable
(same instance, `gc_id` preserved); serialized under the §3 lifecycle protocol. GC's `gc/state` lease is
independent of the mount lease; its destructive protocol is CAS/lease-protected and stale-leader tolerant.

## 7. Introspection, FSCK [A6-part]

- `system.content_addressed_mounts`: non-`store()`-gated lifecycle snapshot — name, lifecycle
  (`live` / `not_live` / `vanished(reason, since)`), last-known identity. `Vanished` disks stay visible.
- **FSCK on a running disk**: dormant-only dropped. **Missing committed manifest** stays a hard finding
  *with* revalidation (re-resolve the exact ref + exact-object HEAD — the sound blob-`Dangling` pattern,
  `CasFsck.cpp:370`). **`meta_without_body` is downgraded to advisory on a running pool** (codex rev.3 #8):
  body-absent + meta-present is a *valid* GC transition (body deleted, meta delete queued async,
  `CasGc.cpp:469`), and a condemned unreferenced blob has no current ref to revalidate — a single ref+HEAD
  recheck cannot make it hard. It hardens only after a stable recheck across a grace ≥ the meta-delete
  horizon. One-row summary unchanged.

## 8. Known limits (explicit non-goals)

- **Cross-session**: a server restart over an erased root re-bootstraps an empty pool (today's documented
  cold-open semantics). Candidate future work: a local marker for a boot-time warning.
- **Partial tampering** (refs erased, `_pool_meta` intact): FSCK's domain.
- **Erase-and-recreate under live processes**: out of contract ([A1], §2).
- **Pre-detection window** (~10 s renewal cadence) after an out-of-contract live erase: mitigated by the
  empty-proof rule (§1) for enumeration-based loads; writes into an erased root in that window can recreate
  keys (S3 PUT creates) — covered by the out-of-contract declaration.
- **Permanently-unconfirmable disk**: visible `DROP` delays until `FORGET`/restart.

## 9. Rollback and tests [A6]

**Rollback of landed Task 4–8**: remove the `MountState` enum + branches (replaced by §1's storage lifecycle
+ pool condition), `UNMOUNT`/`MOUNT` verbs + two `AccessType`s + `ASTSystemQuery` fields + parser/formatter
cases + `unmountSynchronously` + `mountExplicitly` + the drain loop, FSCK-dormant-only, the four lifecycle
gtests (`gtest_ca_transaction.cpp:725` ff.) and the `04290`/`04295`/`05020` teardown patterns. Part-6 sites
are subsumed by the §1 central gate (not merely re-keyed). Keep: Part 1, `poolAccess` snapshot, atomic
`startup()`, `ca-fsck` rename, GC entry-point gating (now via the §3 lifecycle protocol).

**Tests**:
- `05020` + `04290`/`04295`: unique per-invocation disk **names and paths** — `${CLICKHOUSE_TEST_UNIQUE_NAME}`
  **plus an invocation-random suffix** (it is test-basename + database, `shell_config.sh:13`, and does not
  vary under a fixed `--database` rerun — codex rev.3 #10); update all SQL predicates.
- **Teardown: `DROP TABLE ... SYNC` → `SYSTEM CONTENT ADDRESSED FORGET '<disk>'` → `rm -rf`.** `FORGET`
  makes the disk `Vanished` synchronously — no ~11 s transient window racing other tests' sweeps. Never
  `rm -rf` a globally-configured disk's data.
- New tests: identity gate (erase under live mount ⇒ `Vanished`; `SELECT` fails typed; that table's `DROP`
  completes; unrelated `DROP` completes; one WARN then silence); transient (lease key removed/restored ⇒
  throws in the gap, auto-recovers, a `DROP` issued in the gap drains); empty-proof rule (read-only pool
  over erased backing ⇒ ATTACH fails typed, never empty); `FORGET` (live disk: fence tripped, threads
  stopped, `Vanished(forgotten)`, no clean farewell without drain); `GC STOP/START` (rounds stop/resume,
  leader flag cleared, access unaffected).

## 10. Review status

- rev.1 (codex): 3 P0 — resurrect-erased-pool, nonexistent recovery mechanism, unsound STOP. Fixed by:
  identity gate, keeping fresh-incarnation recovery verbatim, dropping STOP.
- rev.2: caller-side typed-catch guard — rejected by owner as Part-6-redux; eject — rejected on formulation
  (re-bootstrap via policy re-resolution; un-droppable tables).
- rev.3 (codex): architecture endorsed; protocol blockers → folded here as [A1]–[A7].
- rev.4: this document. Remaining open items for the implementation plan: exact `ProbeResult` plumbing
  through the backend variants (Native/Emulated/local), the empty-proof rule's cache TTL, FSCK
  `meta_without_body` grace horizon, log-throttling for the DROP re-queue path.
