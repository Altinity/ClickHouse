---
description: 'Live backlog — mount-lease/fence recovery and CA disk lifecycle: startup, decommission, and pool bootstrap.'
sidebar_label: 'Mounts & lifecycle'
sidebar_position: 3
slug: /superpowers/cas/backlog/mounts-and-lifecycle
title: 'CAS Backlog — Mounts and disk lifecycle'
doc_type: 'guide'
---

# CAS Backlog — Mounts and disk lifecycle {#mounts-and-lifecycle}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for mount-lease/fence recovery
and CA disk lifecycle: startup, decommission, and pool bootstrap.

## Mount-lease / fence recovery {#mount-fence}

- **[P3.1 Task 6 / S13] live validation of fence-recovery** — TEST — TLA+ gate PASSED and the correctness paths landed (self-remount on GC fence-out is DONE); the gtest sweep + S13 3×-green live gate remain. **Task 5** (decouple renewal from the retired-view sync beat) is likely **MOOT** — freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`; confirm and close. (An orphaned 2026-08-04-triage finding tracks this same live-validation-under-induced-latency gap — folded in as confirmation.)
- **[A7-residual] gc_scheduler lifetime vs manual rounds** — VERIFY — Believed addressed by `89845c2a544` (shutdown serializes gc_scheduler teardown with health reads; wedged-lane count pinned) on top of the stabilization A7 fix. Confirm no residual: (a) a manual round on a raw pointer captured outside the lock, (b) lazy creation resurrecting a scheduler after shutdown. (An orphaned 2026-08-04-triage finding covers exactly residual (b), lazy-scheduler-resurrection-after-shutdown — folded in as confirmation.)
- **[STID-3982-3b48 part 2] mount-lease self-race Gate 3 re-run still owed** — {#stid-3982-3b48-part-2} — TEST — A third variant of the mount-lease renewal self-race (ambiguous client-side timeout on the renewal `PUT` misdiagnosed as a foreign-writer collision, SIGABRT under ASan) is fixed and landed 2026-07-24 (fence-not-rescue redesign rev.4, TLA+-gated, full `Cas*:CA*` green). Open: Gate 3, the live CAS-s3 stateless-lane validation that originally caught the crash, has not been re-run post-fix — rides the next CI push of `cas-gc-rebuild`.
- **[fence-window observability] mount-lease keeper is silent at default log level** — GAP (found 2026-07-28, fence-cascade RCA) — During the msan CA-s3 fence window (run for `07f8398acddff2c`) the server log contains ZERO `CasMountLease*` lines for the whole ~7-minute episode: renewal failures, the fence arming, remount start/phases/completion are all invisible; the episode had to be reconstructed from `executeQuery` error timestamps. The keeper's messages exist (the STID-3982 entry above quotes them from an ASan run) but evidently sit below the effective level or fire only on classifier paths. Fix: log at `Information` (rate-limited) — renewal confirm failure with the underlying error, fence armed (with deadline), remount begin, remount recovery milestones, remount complete with duration. Cheap, pure logging, closes gap #3 of `reference_cas_ci_observability_gaps`.
- **[fence-window blast radius] durable writes fail instantly for the whole fence→remount window** — DESIGN QUESTION (2026-07-28 RCA) — During fence→remount (~2 min core window + straggler tails on the msan lane), every durable write returns `668`/`210` immediately; user queries (test INSERTs) get hard errors while an internal `CasWriteRetryLater` lane already exists for system-table flushes. A bounded wait-for-remount on the query write path (block up to N seconds while the self-remount is in flight, then fail) would turn short fence windows into latency instead of failures — the same contract RMT gives during a Keeper reconnect. Behavior change on the write path — needs a design decision, do NOT slip it in as a patch. Related: the remount itself took ~2 min under msan; once the keeper logging (entry above) lands, measure WHERE remount time goes (lease-expiry wait vs recovery replay) before tuning anything.
- **[B208] CA startup mount-probe is fail-closed against a transient S3 outage — server aborts and stays down** — DESIGN QUESTION — A server started while the object store is unreachable dies during metadata load (mount startup capability probe times out, exit 243, no retry, stays down until an operator restarts it). Product question: bounded startup retry / degraded-start (mount later, serve non-CA tables meanwhile) vs. fail-closed correctness (a server that starts without its pool must not fake readiness). Not a gate for the durability fix (S40's contract only requires acked data to survive on the live replica). When fixed, add an informational recovery verdict to S40 so a regression here produces signal.
- **[POOL-REFUSAL-NODE-FATAL] a pool bootstrap refusal takes the whole node down** — {#pool-refusal-node-fatal} — DESIGN QUESTION (2026-07-29, surfaced by the W3 RCA; pre-existing bootstrap behaviour) — the residual-data guard (`CasPool.cpp` ~:439, Code 668 `missing _pool_meta over a non-empty pool prefix`) raises during metadata loading and propagates out, so the SERVER EXITS (container exit 156) instead of starting with that one disk marked unusable. Refusing the pool is right (fail-close); taking the node down for one residual CA prefix is the question — a node may serve many disks/tables that are healthy. Direction: bootstrap-refusal -> disk marked broken/read-refused + loud diagnostics + the node UP, consistent with the disk-lifecycle redesign goals (UNMOUNT ejects, FSCK not dormant-only); the refusal message already names the operator verbs (recreate or restore `_pool_meta`). Evidence: S43's W3 answer (refusal + causation control).

## CAS disk lifecycle rev.8 round (FORGET-only) — residuals {#disk-lifecycle-rev8-closure}

Round: rev.8 (FORGET-only), problem framing goals G1–G7. G1-G5
resolved this round (isolation fix, throw-not-abort, GC self-exit on Vanished/IdentityLost, generic-code
correctness, FSCK-on-running advisory).

**NOT resolved (deliberately deferred):** the underlying **disk-lifecycle-leak** proper — a CA disk is still
cached forever in the disk registry (`Context::getOrCreateDisk`) with no teardown/eject on `DROP TABLE`, and
there is no runtime re-use of the same disk after a stop (G6 is met only node-locally via `FORGET`; G7
abandoned). The Dormant/UNMOUNT/MOUNT reuse machinery that pursued this was rolled back (spec rev.8 §9);
`FORGET` is the node-local decommission story. Full eject-on-`DROP` is future work (the disk-lifecycle
redesign; v2 door in git history). Three orphaned 2026-08-04-triage findings describe the same disk-registry-caches-forever-after-DROP-TABLE class (including a manual-GC/rebuild-vs-shutdown race variant) — folded in here as confirmation, no new content.

**Accepted residuals / watch items (each a pointer into this round):**
- (a) **`search_orphaned_parts_disks=ANY` × a transient CA disk strands an unrelated table's load** —
  ACCEPTED (spec §4 blast radius). With `search_orphaned_parts_disks=ANY` the orphaned-parts sweep touches
  every disk, so a transient / `IdentityLost` CA disk makes an unrelated table's AsyncLoader load throw, and
  AsyncLoader does not retry-on-touch → the table stays FAILED until a manual `ATTACH`. Cure: `ATTACH` (or
  restart); guidance: keep `search_orphaned_parts_disks=LOCAL` when a CA disk may be transiently unreachable.
- (b) **Teardown/shutdown-window fail-loud** — NOTE (plan Task 15; spec §1/§3). Null-pool access
  (`Constructing`/`ShutDown`) is now FAIL-LOUD (`INVALID_STATE`), including the `Probe` class. A generic
  all-disks sweep racing table/server shutdown now sees a throw from the CA disk rather than a silent empty —
  intended (fail-loud > silent-skip; the old T8a null-pool wedge is structurally gone), but watch for
  benign-but-noisy shutdown-window throws in sweeps.
- (c) **GC `start()` partial-start desync guard (pre-existing)** — DEFERRED (T11 review, M4). `gcStart`'s
  re-enter of the scheduler `start()` has no guard against a partial-start desync (a worker/heartbeat pair
  left half-started, leaving the started/stopped flag inconsistent). Pre-existing, out of this round's scope;
  carried for a future GC-scheduler hardening pass.
- (d) **`RefWriter` DeathTest fork-under-load flake** — WATCHED (fix1 review, `1fe585ea078`). A `RefWriter*`
  `EXPECT_DEATH` test's `fork()` failed once (~1 ms) under full parallel gate load; 3/3 green isolated and on
  clean re-run. Class = fork-under-load, not a product red. Watch for recurrence; if it recurs, serialize the
  CAS DeathTests or lower gate parallelism around them.

## Operator recovery: mounting a pool whose owner uuid differs — not decided, not started {#operator-uuid-recovery}

A server whose local uuid file was regenerated (wiped data dir, a pod recreated without a persistent
volume) cannot mount its own pool. `CasServerRoot.cpp:120-131`'s refusal already names three manual
recoveries (restore the uuid file, configure a fresh `server_root_id`, or delete the owner object by
hand after verifying no server uses the root); a supported command would automate the third. **Open
design choice**: overwrite the owner uuid with a new one (works, but permanently locks the original
server out, and must cover both the owner and mount objects to keep the epoch-1 re-mint guard armed),
or adopt the pool's existing owner uuid and mount as it (`Pool::openForDecommission`,
`CasPool.cpp:720-776`, already does exactly this — the reading that looks strictly better). Not the
read-only-mount task, which is separate and unimplemented.

Three orphaned 2026-08-04-triage findings target this same open design question: one proposes a
concrete `force_owner_claim` mechanism (not found anywhere in `src/` — a proposal, not a partial
implementation); one asks for an audit record on any force-claim path; one asks the design to
distinguish post-mortem recovery from live-server misuse. None change the open status above — folded
in as detail on the same unimplemented design choice.

## Removing and re-adding a replica under a k8s operator walks into an unrecoverable identity refusal (field report 2026-09-04) {#operator-replica-readd-uuid-trap}

**HARD.** Field evidence, not a review finding: a `clickhouse-operator` cluster (`chi-cas-demo`) had
replica `cas-demo-0-1` removed one day and re-added the next. The `server_root_id` is derived from the
pod name and is therefore stable, while the pod's local `uuid` file (`<path>/uuid`, written by
`ServerUUID::load` in `programs/server/Server.cpp:1753`) was regenerated because the data dir did not
survive. `claimOwnerOrThrow` (`CasServerRoot.cpp:547`) then refuses with
`CAS server-root '...' is owned by a different server (owner server_uuid=..., ours=...)`, raised during
startup metadata load — the same node-fatal class as [`[POOL-REFUSAL-NODE-FATAL]`](#pool-refusal-node-fatal).
This is the concrete instance of the open design choice recorded in
[operator recovery above](#operator-uuid-recovery); what follows is what the field run added to it.

**1. Our own refusal message leads the operator into a strictly worse state.** The third recovery it
names — "manually delete the owner object `.../gc/server-roots/<srid>/owner` and restart" — is only
valid when that root never held data. Over a non-empty subtree the next claim hits the second refusal,
`has no owner anchor but its data subtree is non-empty (identity lost over existing data)`
(`CasServerRoot.cpp:559`), which offers **no** recovery advice at all. The operator followed the
message and landed there. Minimum fix: qualify the delete-owner advice, name
`SYSTEM CAS DROP POOL MEMBER` / `clickhouse-disks cas-drop-member` as the supported verb in both
messages, and give the identity-lost refusal a recovery sentence of its own.

**2. The hand-delete also destroys the cheapest recovery input.** `Pool::openForDecommission`
(`CasPool.cpp:907-919`) resolves the victim uuid from the owner anchor and falls back to the mount
lease. An operator who deletes both by hand — a plausible next step after the advice above fails —
gets `unknown pool member (no owner anchor and no mount lease)`, and the victim's `roots/<srid>/`,
`cas/manifests/<srid>/` and its catalog namespaces are then unreachable by every supported command.

**3. Decommission does not make the srid reusable either.** The retirement tail tombstones the owner in
place (`CasDecommission.cpp:446`: `retired_at_ms` set, `server_uuid` **unchanged**). A pod later
recreated with the same `server_root_id` and a new uuid does not even reach `throwIfOwnerRetired` — the
uuid comparison fires first and it gets the same "owned by a different server" wall. So the sequence a
k8s operator would naturally run (`SYSTEM DROP REPLICA` + `SYSTEM CAS DROP POOL MEMBER`, then re-add the
pod) does not work today. Only the accidental path — owner hand-deleted, subtree then emptied by
decommission — leaves a claimable slot, which is the opposite of what the messages recommend.

**4. The ask (user, 2026-09-04).** "Оператор уже делает `SYSTEM DROP REPLICA`, должно быть что-то
похожее для CAS." A supported replica-removal verb the operator can call, plus the guarantee this entry
adds to the [open design choice](#operator-uuid-recovery): **after the supported removal, re-adding the
same `server_root_id` with a new `server_uuid` must succeed without hand-editing the object store.**
Whichever of the two options is chosen (rebind the owner uuid, or adopt the pool's existing uuid),
that is the acceptance criterion. Until it exists, the only advice that actually holds for k8s is
"keep the `uuid` file on a persistent volume", and it is written down nowhere operator-facing —
`operations/migration.md#decommission` documents permanent removal only, not remove-and-re-add.

**Interim recovery that does work** (record it in the docs as such until the verb exists): with the
victim's mount object still present, run `SYSTEM CAS DROP POOL MEMBER '<srid>' FROM DISK '<disk>'` from
a surviving pool member (or `clickhouse-disks --disk <disk> cas-drop-member '<srid>'` offline) **while
the victim is down**; with the owner anchor already gone, the run adopts the uuid from the mount lease,
empties the subtree, and reports a `slot tombstone failed: ... object absent before tombstone write`
warning — harmless here, and precisely the absence that lets the re-added pod claim the srid cleanly.

## `life_epoch` monotonicity holds PER SERVER ROOT — decommission must not break it {#life-epoch-monotone-per-server-root}

Recorded 2026-07-31 from Task 4c, which made a decreasing `_ckpt.life_epoch` contribution `CORRUPTED_DATA`
instead of letting `max` absorb it. That refusal rests on an argument with a stated limit, and the limit is
what this entry exists for.

`writer_epoch` is durable-monotone **per server root** — `allocateWriterEpoch` CAS-bumps
`<prefix>/gc/server-roots/<srid>/epoch` — and every live namespace is rooted at its own member's
`server_root_id`, so a creator and any actor that later reconciles it draw from **one** counter. That is what
makes "contributions only ever rise" true, and therefore what makes a decrease a fenced-out writer rather than
an ordinary race.

**If a namespace could ever be created by one server root and later have its `_ckpt` contributed to by
another, the argument fails**: the two counters are independent and unordered, so an honest contribution from
the second root could be numerically lower and would be refused as corruption. Nothing does that today.

**Pool-member decommission is where this would be introduced**, since moving or adopting a namespace across
roots is exactly the shape. Whoever owns that work must either keep a namespace's `_ckpt` contributions within
one root for its whole life, or replace the monotonicity argument with something that survives two counters —
and must not discover this by hitting the refusal. The limit is stated at `joinLifeEpoch` in the code as well,
so the constraint is visible where it is relied upon rather than only here.

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[decommission-successor-mount-race] decommission can delete a successor's fresh mount/epoch objects after releasing an impersonated lease** — HARD — Review finding №9: a durable epoch-monotonicity violation. No evidence this specific race was closed — distinct from the general decommission two-phase-heal flow (verified separately), which does not address this successor-race angle.
- **[gc-scheduler-lazy-init-race] ✅ CLOSED at HEAD by `452d17af42f` + `e79a109b142` (verified 2031-triage CAS-050); kept for provenance** — was MINOR — lazy creation now happens under `pointer_mutex` inside `gc_scheduler_mutex`. Original text: Concurrent `SYSTEM GC` calls could race construction; a plausible narrow concurrency bug, distinct from `[A7-residual]` above (which is about post-shutdown resurrection, not concurrent first-construction).
- **[fence-costs-epoch-distinct-mint] mint `epochCeiling + 1` instead of literal `1` in `RemintEpoch`'s honest branch** — DESIRABLE — Its own source (`CaCasMountCore_RESULTS.md`) explicitly flags this as "NOT implemented... a candidate refinement for a later task/round" that would close the documented `FenceCostsEpoch` gap.

## `CasGcScheduler::stop` joins the worker threads outside the mutex that guards them (2031-triage CAS-050) {#gc-scheduler-stop-join-race}

`stop()` takes `mutex` only to set `stopping`, then touches the thread objects without it
(`CasGcScheduler.cpp:82-85`: `if (thread.joinable()) thread.join(); if (hb_thread.joinable())
hb_thread.join();`), while `start()` (`:67-72`) and `requestRoundSoon()` (`:98-101`) read/write the same
`thread`/`hb_thread` members under `mutex`. `ThreadFromGlobalPool::join` does `state.reset()` and
`joinable()` reads that same `shared_ptr` (`src/Common/ThreadPool.h:390-410`), so the overlap is a real
data race (UB, and a TSan red), not a formality.

`start()`/`stop()` are mutually serialized on every published path by `gc_scheduler_mutex` (+
`lifecycle_mutex` for the verbs), so the reachable pair is `requestRoundSoon` vs `stop`:
`ContentAddressedMetadataStorage::requestGcRoundSoon` takes only `pointer_mutex`
(`ContentAddressedMetadataStorage.cpp:419-428`) and its sole caller is `SYSTEM CAS DROP POOL MEMBER`
(`src/Interpreters/InterpreterSystemQuery.cpp:1077`); `SYSTEM CAS GC STOP` deliberately leaves the
scheduler in the member, so the window stays open for its whole `stop()`. Needs two concurrent operator
actions (or `DROP POOL MEMBER` racing server `shutdown()`), no data loss and no silent corruption — hence
P2. Fix: move the thread objects into locals under `mutex` (or add a dedicated join mutex) and join
outside the lock.

The audit's second half — a self-exited loop leaving a "joinable-but-dead scheduler that reports itself
as running" — is not a defect: self-exit fires only on `isVanished()` /`vanishedIntentPublished()` /
`IdentityLost` (`CasGcScheduler.cpp:281-282`, `:369-370`), `gcStart` refuses loudly on exactly those
states via `checkOpAdmitted(Admin)`, and `i_am_leader` is cleared before the return so `gcHealth` stays
honest.

## `type=encrypted` over a CAS disk is accepted at startup with no capability gate (2031-triage CAS-059) {#encrypted-over-cas-missing-gate}

CAS + encryption is deliberately out of scope for this release (BACKLOG/operability-and-introspection.md
`[B17]` covers the real feature: a per-encryption-key dedup scope). The residual is only the MISSING
FAIL-FAST: `<disk><type>encrypted</type><disk>cas_disk</disk></disk>` is accepted without any check.
`registerDiskEncrypted`'s creator takes any `DiskPtr` out of the disk map
(`src/Disks/DiskEncrypted.cpp:517-531`, `getDiskAndPathFromConfig` `:190-208`) and `DiskEncrypted` does
not delegate the two CAS capability answers — it forwards `isPlain` (`src/Disks/DiskEncrypted.h:332`)
but neither `isContentAddressed` nor `supportsAtomicFileWrites`, so both fall back to the `IDisk`
defaults `false` (`src/Disks/IDisk.h:473-480`) while the delegate answers `true`
(`.../ContentAddressed/ContentAddressedMetadataStorage.h:261`). Consequences: (a) every CAS-aware
MergeTree branch is switched off (`DataPartStorageOnDiskBase.cpp:542`, `:735`,
`MergeTreeData.cpp:7544`, `DataPartsExchange.cpp:161`, `:405`), (b) `createTransaction` returns a
`FakeDiskTransaction` by default (`DiskEncrypted.h:344-349`, `use_fake_transaction` defaults to true —
`DiskEncrypted.cpp:329`), so every part file reaches the CAS layer as a standalone autocommit write and
the first blob-mandatory file throws `NOT_IMPLEMENTED`
(`ContentAddressedTransaction.cpp:766-771`), (c) with a non-empty `<path>` prefix
(`DiskEncryptedTransaction::wrappedPath`, `src/Disks/DiskEncryptedTransaction.h:36-42`) FREEZE shadow
classification stops matching, because it requires the LITERAL first component `shadow`
(`Parts/PartPathParser.cpp:277-282`) — part-file classification itself is prefix-robust (shape-based
uuid anchor, `Parts/PartPathParser.cpp:114-128`).

So the failure is loud and immediate at the first INSERT, never silent corruption — hence a P3 hygiene
item, not a release blocker. Fix (small): refuse at disk construction/config validation when the
delegate (transitively) answers `isContentAddressed()`, with a message naming CAS+encryption as
unsupported; optionally also forward `isContentAddressed`/`supportsAtomicFileWrites` in `DiskEncrypted`
once the combination is actually designed.

## An owner-only slot (mount and epoch already deleted) has no row in `system.content_addressed_mounts` (2031-triage CAS-063) {#owner-only-slot-invisible-in-mounts}

P3, observability only — the state is repairable, and the repair is re-running the same command.

`Cas::listMounts` enumerates `roots/` and keeps only keys ending in `/mount`
(`ContentAddressed/Pool/CasServerRoot.cpp:754-763`), and `system.content_addressed_mounts` is built
from it (`src/Storages/System/StorageSystemContentAddressedMounts.cpp:163`). So the window a
decommission crash can leave — `mount` and `epoch` deleted
(`Tools/CasDecommission.cpp:372-373`), the owner anchor not yet tombstoned (`:408-435`) — produces a
member with objects in the pool and no row in the mounts view. An operator surveying the pool cannot
see that the slot is half-retired.

It is NOT unrepairable, contrary to how the finding this came from stated it: the owner anchor is
itself the resume anchor (`Pool::openForDecommission` reads it, and falls back to the mount lease when
it is gone, `Pool/CasPool.cpp:820-828`), and an absent `epoch` under an absent/terminal mount is the
`EpochMintPolicy::DecommissionRecovery` path (`Pool/CasServerRoot.cpp:221-247`), which mints a fresh
distinct epoch and lets the re-run reach the retirement tail. Pinned by
`gtest_cas_decommission.cpp:1054` (`SuccessorReclaimAfterEpochDeleteKeepsOwnerAnchor`), `:1231`,
`:1267` and `:1325` (`MidRetirementCrashResumesViaMountLeaseFallback`).

Owed (small): give the mounts view a row for a slot that has an `owner` (or an `epoch`) but no
`mount`, with a state naming it — `retiring`/`half-retired` — so the operator sees the resume anchor
instead of a silence. Related: {#decommission-successor-mount-race} (the other end of the same
retirement tail) and `BACKLOG.md`{#decommission-wrong-predicate}.

## CLOSED — Recovery-thread spawn could permanently disable self-remount (2031-triage CAS-070) {#remount-running-latched-before-spawn}

Closed 2026-08-24 by the runtime-ownership work in `ecf3d5d7c76f` and its review fixes. Writable
startup now constructs both long-lived runtime-owned workers before publishing the pool; a partial
construction failure joins the created worker, closes the fence, and fails the mount. Incident paths
only increment a monotonic remount-generation latch and wake the persistent worker, so
`scheduleRemount` no longer allocates a thread and cannot strand a `remount_running` flag. Teardown
requests stop and joins both workers. Deterministic construction-failure and generation-latch tests,
plus the 15-minute S39 remount campaign, cover the replacement contract.

## The mount-fence clock uses `CLOCK_BOOTTIME` with no portability shim, so the CAS sources do not compile on Darwin (2031-triage CAS-092) {#boottime-not-portable}

Two unconditional `clock_gettime(CLOCK_BOOTTIME, ...)` reads sit in code that is compiled on every
platform: `Pool/CasMountRuntime.cpp:63` (`CasMountRuntime::bootMs`, the write-fence clock) and
`Pool/CasServerRoot.cpp:58` (`defaultBootMs`, the keeper's boot anchor). Both files are added to
`dbms` unconditionally (`src/CMakeLists.txt:139`) with no `OS_LINUX` guard and no include of a
compatibility header.

`CLOCK_BOOTTIME` is a Linux name. Darwin's `<time.h>` does not define it (its boot-domain clock is
`CLOCK_UPTIME_RAW`, which unlike `CLOCK_BOOTTIME` *excludes* sleep, so it is not a drop-in for the
fence's stated requirement in `Pool/CasMountRuntime.h:68-72`); FreeBSD defines it as an alias of
`CLOCK_UPTIME` under `__BSD_VISIBLE`, so only the Darwin builds are expected to break. The failure
is a compile error, maximally loud, with zero runtime or correctness exposure — but the darwin build
jobs will be red the first time the branch runs them.

The shim precedent already exists: `base/base/time.h` maps `CLOCK_MONOTONIC_COARSE` per platform.
The owed treatment is the same shape — extend that header (or a CAS-local one) with a
`CLOCK_BOOTTIME` mapping for Darwin, and state in the mapping comment that Darwin's substitute does
not include sleep time, which weakens (does not break) the suspend argument the fence relies on.

## `decodeServerEpoch` accepts `nwe = 0`, and `MountFence` carries two never-read identity fields (2031-triage CAS-130) {#server-epoch-zero-and-dead-fence-identity}

Two small hardening/hygiene residuals around the writer-epoch fence. Neither is a live defect: both
end fail-closed or inert, and both are cheap to close.

1. `Formats/CasServerRootFormats.cpp:91-117` (`decodeServerEpoch`) validates only that the `nwe` key
   is present — a body with `nwe = "0"` decodes to `ServerEpoch{next_writer_epoch = 0}`. On the
   object-PRESENT path of `allocateWriterEpoch` (`Pool/CasServerRoot.cpp:265-272`) there is no
   zero-clamp (the clamp exists only on the absent path, `Pool/CasServerRoot.cpp:263-264`), so that
   decode would hand out `writer_epoch = 0`. No writer can produce such a body (the only encoder site
   writes `next + 1`, `Pool/CasServerRoot.cpp:269-272`), so this needs a corrupted or hand-written
   `cas_epoch` object; and the consequence is loud, not silent — the first ref transaction /
   manifest ref encode of the incarnation throws `CORRUPTED_DATA` on the nonzero-epoch check
   (`Formats/CasRefWireVocab.cpp:33-37`, `Formats/CasWireVocab.cpp:87-99`). `CasDecommission` already
   treats `next_writer_epoch != 0` as a validity precondition (`Tools/CasDecommission.cpp:335`), so
   the owed treatment is to reject zero at decode time, where the other epoch invariants live.
2. `MountFence::server_uuid` / `MountFence::writer_epoch` (`Pool/CasMountRuntime.h:76-77`) are
   assigned by `armMountFence` (`Pool/CasMountRuntime.cpp:136-137`) and read by nothing, in the whole
   tree. The local fence is deliberately a latch + deadline + generation triple (`mayMutate`,
   `Pool/CasMountRuntime.cpp:80-84`; `checkFenceOrThrow`, `:97-113`), and the durable identity is
   enforced elsewhere — the keeper's token-guarded CAS and the `liveWriterEpoch` comparison in
   `Pool/CasPartWriteTxn.cpp:158-161`. Either drop the two fields, or keep them and say in the
   comment that they are a diagnostic record only, never an admission input.

## A failed `listMounts` is not distinguishable from "live pool with no slots" in `system.content_addressed_mounts` (2031-triage CAS-133) {#mounts-list-failure-indistinguishable}

P3, observability only — nothing is corrupted and the failure is already logged.

When `Cas::listMounts` throws, `StorageSystemContentAddressedMounts::read` logs it
(`src/Storages/System/StorageSystemContentAddressedMounts.cpp:170-172`), sets `list_ok = false`, and
falls through to the synthesized snapshot row (`:223-242`). That row still carries the truthful
non-gated lifecycle (`appendLifecycle(snap)`, `:241`), so the disk does NOT look absent and does NOT
look like a never-started disk: a healthy pool keeps `lifecycle = 'live'` while a never-started disk
reports `constructing` and a torn-down one `shutdown`
(`ContentAddressedMetadataStorage.cpp:468-474`). What the row cannot express is *why* the lease
columns are blank: `state` is a non-nullable `String` and gets `insertDefault()` (`:236`), so a
throttled LIST renders exactly like a `live` pool that legitimately listed zero slots.

Owed (small): either give the synthesized row a `state` word naming the cause (e.g. `unknown` when
`list_ok = false` versus `''` for a genuinely empty listing), or add a nullable
`mount_list_error` column carrying `getCurrentExceptionMessage`. Related:
{#owner-only-slot-invisible-in-mounts} (the other reason a slot has no row).

## `~Pool`'s network teardown runs under `pointer_mutex` (umbrella review M8) {#pool-dtor-under-pointer-mutex}

In `ContentAddressedMetadataStorage::shutdown()` both `part_access.reset()` and `cas_store.reset()`
still execute while `pointer_mutex` is held. With GC disabled or a read-only mount — real
configurations — `cas_store` is the last strong reference, so `~Pool`'s multi-second network teardown
(ref-lane drain plus the farewell write) runs inside the lock and blocks every snapshot reader
(`poolAccess()`, `store()`, `gcHealth()`, `system.cas_mounts`). The GC-enabled case is masked only by
the scheduler holding a second reference — a refcount accident, not an invariant, and `pointer_mutex`
is documented as a brief-snapshot lock. Fix: hoist both into locals, release the lock, reset them
unlocked — exactly the handling `old_scheduler` already gets in the same function. P3: latency and
lock-hold hygiene, no corruption.

## `createNamespaceStep1` is the one writer-plane durable write without a fence check (umbrella review V1) {#create-namespace-step1-unfenced}

Result of the exhaustive sweep the 2026-08-05 review asked for and could not finish: every mutating
`Cas::Backend` method plus the one non-backend `writeObject` staging path was enumerated and
classified (writer plane / GC plane / administrative / bootstrap). Exactly one writer-plane site
lacks the fence: `createNamespaceStep1` (`Pool/CasRefCatalog.cpp:200-230`) is the only caller of
`casUpdateImpl` that skips the fence obligation its own header documents
(`Pool/CasRefCatalog.h:84-95`), while the parent `createNamespace` passes
`admitted_generation`/`check_fence_or_throw` to step 3.

Consequence: a fenced-out mount can durably insert a `Creating` row carrying a freshly minted
namespace incarnation under an already-dead `CreatorFence`. It self-heals — step 3 refuses and the
next opener reconciles — so this is durable garbage in the pool-global `cas/ref_catalog`, not data
loss, hence P3. The fix is one line, symmetric with the four sibling callers, and there is no test
for it today (`gtest_cas_fence_generation.cpp` covers the plain-object, S3-staging and condemned
paths only). Lower-confidence secondary: the abort-path `deleteExact` at
`Pool/CasPartWriteTxn.cpp:1433` is unfenced and sits inside a swallowing `catch(...)`.

## Decommission liveness recheck vs owner-anchor CAS: a live successor can be stamped retired (umbrella review V4) {#decommission-toctou-stamps-successor}

The review's framing — "the sweep deletes what the successor needs" — does not hold: the destructive
phases run under the victim's own mount lease, released only at `admin.reset()` afterwards. The
window is real but produces a different defect: a cross-key TOCTOU between the liveness recheck (the
`mount`/`epoch` GETs) and the owner-anchor CAS lets a LIVE same-UUID successor be stamped
`retired_at_ms`, so its next restart is wrongly refused with `CORRUPTED_DATA`. Availability, not
loss; reproducing it needs a chaos test that restarts the victim between the recheck and the CAS.
P2. Loosely adjacent: 2031-triage CAS-063 and CAS-007.

## Detached CAS work can outlive `Context`: `~Pool`'s farewell logs through a null shared context (opus review B3+B4) {#detached-pool-outlives-context}

One coupled defect chain, both halves verified at HEAD and neither tracked before:

- **B4** — `shutdown()` does not drain the detached CAS dispatches, and they hold a STRONG reference
  to `Pool`. The snapshot publisher is a routine path, not an anomalous one, so a detached task can
  legitimately be the last `Pool` owner; `~Pool` then runs its durable farewell write and emits a
  mount event arbitrarily late — potentially after the object storage is already shut down. rev.8
  closed the neighbouring half (GC threads self-exit on a terminal pool), not this one.
- **B3** — CAS holds a strong `const ContextPtr` and logs through it. `Context::getContentAddressedLog`
  dereferences `shared` unconditionally, while `resetSharedContext()` nulls it before that last
  farewell `MountRelease` fires. So the tail of B4 lands on a null `shared` — a null dereference at
  shutdown, on a background thread.

Fix shape: give the pool's detached work a tracked drain that `shutdown()` waits on (so `~Pool` runs
while the world still exists), and make the event-emit path tolerate an absent log/context instead of
dereferencing it. Both are needed: the drain removes the ordering, the tolerance removes the class.
P1 — a shutdown-path null dereference is a crash on every server that ever mounted a CAS disk.
