---
description: 'Design for removing a dead replica (pool member) from a content-addressed pool: SYSTEM CONTENT ADDRESSED DROP POOL MEMBER and the ca-drop-member disks command over a shared decommission core'
sidebar_label: 'CAS Pool Member Decommission'
sidebar_position: 20260713
slug: /superpowers/specs/cas-pool-member-decommission-design
title: 'CAS Pool Member Decommission Design'
doc_type: 'reference'
---

# CAS pool-member decommission (dead-replica removal) {#cas-pool-member-decommission}

**Date:** 2026-07-13
**Branch:** `cas-gc-rebuild`
**Status:** design (brainstormed + section-by-section approved by user 2026-07-13)
**Supersedes:** the deferred B200 sketch in `docs/superpowers/cas/05-formats-and-backend.md`
§deferred-rollout ("`SYSTEM DROP CONTENT ADDRESSED POOL MEMBER`, needs roster first") — this design
implements deliberate decommission WITHOUT the durable roster (which still does not exist), and renames
the grammar to the `SYSTEM CONTENT ADDRESSED …` subsystem-prefix family.

## Motivation {#motivation}

There is no path today to remove a permanently dead replica from a CAS pool. The heartbeat fence pass
marks an expired mount `gc_fenced` (`computeHeartbeatFloor`, `CasServerRoot.cpp`) and a graceful stop
stamps the farewell sentinel (`min_active == UINT64_MAX`), but nothing ever erases what the member owns.
A crashed-and-never-returning replica pins, forever:

- **Its frozen watermark blocks the orphan-manifest sweep of its own debris.** `prefixEligible`
  (`CasOrphanManifestSweep.cpp:146-163`) compares against the mount body's `{writer_epoch, min_active}`;
  a crashed member froze `min_active` at its last beat, `gc_fenced` does not feed eligibility, so its
  pre-precommit manifest debris at the same epoch is never sweep-eligible.
- **Its stale precommits protect manifests and blobs forever.** Dead-precommit cleanup is the successor
  writer's job (snapshot+log ref model: "`GC` never invents a ref transition", `CasGc.h:185-187`); a
  namespace no writer ever mounts again keeps its precommit-protected closure alive indefinitely.
- **Its committed refs keep every referenced blob alive** — the intended fail-safe, but for a
  decommissioned server it is permanent storage leak by design absence.
- **Its staging prefix** (`staging/<mount_id>/`) — `sweepOwnMountStaging` is the sole reclaimer and only
  the owning mount runs it.
- **Its mountpoint objects** (`roots/<srid>/…`, `CasLayout.h:306-318`) — loose non-content-addressed
  files outside every namespace; nothing sweeps them.
- **Its mount-slot subtree** (`gc/server-roots/<srid>/{mount,owner,epoch}`) — fenced/terminated slots
  are terminal and deliberately preserved for S13 same-uuid recovery.

Post-Phase-B (freshness-meta) GC no longer waits on any replica — a dead member does not stall
graduation — so the remaining problem is purely the permanent footprint above. This design gives the
operator one deliberate, safe command that erases all of it.

## Decisions (from the design interview) {#decisions}

| Question | Decision |
|---|---|
| Scope | **Full erasure** — namespaces (committed refs + precommits), manifest debris, staging, roots objects, slot. Blob bytes are reclaimed by subsequent normal GC rounds. |
| Liveness gate | **Token-guarded slot claim** = the gate (same code path as S13 self-remount). A live member wins its lease renewals → claim refuses. **No FORCE variant.** |
| Granularity | **Whole srid** (pool member). Per-table cleanup on a LIVE replica already exists — ordinary `DROP TABLE`. |
| Surface | **SQL + disks tool over one shared core** (`Core/CasDecommission.{h,cpp}`). |
| Preview | **No dry-run.** The command executes directly and returns a summary of what was deleted. |
| Grammar | `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '<srid>' FROM DISK '<disk>'` — subsystem-prefix family (precedent: `SYSTEM JEMALLOC …`), spelling matches `metadata_type=content_addressed` and the `system.content_addressed_*` tables. All future CAS verbs share the `SYSTEM CONTENT ADDRESSED` prefix. |

Rejected execution alternatives: (B) decommission-intent marker executed by GC — requires GC to invent
ref transitions, explicitly forbidden by the ref-table model; (C) raw prefix deletion + mandatory
`ca-gc-rebuild` — breaks in-degree accounting until an O(universe) rebuild runs. Chosen: **(A) the
command becomes a temporary writer** and drives only existing, month-hardened writer machinery.

## Core: `Cas::decommissionPoolMember` {#core}

New `Core/CasDecommission.{h,cpp}`; one orchestrator returning a `DecommissionReport`. Inputs: the
pool `Backend`, `Layout`, pool config, the victim `server_root_id`, an event sink. Steps — each an
existing mechanism, in this order:

1. **Gate + claim.** Plain `claimMount` (NOT `claimMountAwaitingExpiry` — no waiting): a live lease ⇒
   immediate refusal `"pool member '<srid>' is alive (lease expires at …, uuid …, hostname …)"`.
   Success bumps `writer_epoch`. For the duration of the run a standard `MountLeaseKeeper` beat renews
   the claimed lease so the GC heartbeat pass counts us live and does not fence us mid-erase.
   Self-decommission is refused by the same gate (the executing server's own lease is live).
2. **Namespace enumeration.** Scoped `LIST cas/refs/<srid>…` with the same key parsing GC discovery
   uses (no registry exists — D1). Already-`Removed` namespaces are skipped (idempotence/resume).
3. **Per-namespace erasure.** Standard recovery (snapshot+log replay) → `Store::dropNamespace(ns)`
   (`CasStore.h:409`): exact `owner_transition(→none)` for every committed ref AND precommit, then
   `remove_namespace`, then the constant-size `Removed` snapshot. Stale precommits die here (recovery
   enumerates them). The `-1` edge deltas flow to GC normally; the physical `@cas@` namespace
   (manifest bodies + verbatim files such as `format_version.txt`) is reclaimed by the existing GC
   namespace-cleanup item (`CasGc.cpp:1102`).
4. **Manifest-debris drain.** Before the slot can be deleted, residual pre-precommit manifest debris
   under the victim's namespaces is drained by the command itself: deleting the mount body would
   destroy the watermark authority (`floorForNamespace` → nullopt → "no watermark ⇒ not eligible")
   and strand the debris forever. At this point the command IS the epoch authority (every old-epoch
   prefix is eligible: `prefix.writer_epoch < w.writer_epoch`), so a scoped sweep over
   `cas/manifests/` for the victim's namespaces removes the rest by exact token.
5. **Staging sweep.** `sweepOwnMountStaging` (`CasStagingSweeper.h`) over the claimed victim mount's
   `staging/<mount_id>/` prefix.
6. **Roots sweep.** Scoped `LIST roots/<srid>/` → delete every mountpoint object
   (`mountpointObjectKey`, `CasLayout.h:312`; keys are built as `serverPrefix() + "/" + path`,
   `ContentAddressedMetadataStorage.cpp:1057`). These carry no tokens and no epoch, but the victim's
   writers are fenced by the claim, so no write can race the sweep.
7. **Slot retirement.** Farewell-stamp (`min_active = UINT64_MAX`) → delete
   `gc/server-roots/<srid>/{mount,owner,epoch}`. The slot is deleted STRICTLY LAST — it is the resume
   anchor: a crashed command re-runs, re-claims the (terminated/expired) slot, skips `Removed`
   namespaces, and finishes.

**Fail-close:** if any drain step (4-6) cannot confirm emptiness (transient backend errors), the slot
is NOT deleted — it stays terminated with the farewell stamp, the report carries a warning, and a
re-run completes the job. Never delete the resume anchor over an unconfirmed drain.

The command does not wait for blob reclamation: bytes become unreferenced when the edge deltas fold,
and physical deletion follows the normal condemn → two-phase-graduation pipeline. The report says
"unreferenced", never "freed".

## Surfaces {#surfaces}

### SQL {#sql-surface}

```sql
SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '<srid>' FROM DISK '<disk>'
```

- Parser/AST: new `ASTSystemQuery` type with two string arguments (srid, disk name).
- Access control: new `AccessType SYSTEM_CONTENT_ADDRESSED_DROP_POOL_MEMBER` under the SYSTEM group
  (modelled on `SYSTEM DROP REPLICA`).
- Interpreter: `InterpreterSystemQuery` resolves the disk by name, validates
  `metadata_type = content_addressed`, extracts `Backend`/`Layout`/pool config from its metadata
  storage, calls the core. The executing server's own mount is untouched — the core performs a
  separate admin claim of the victim srid over the same backend connection.
- Synchronous execution inside the query (long-running is acceptable — like `SYSTEM SYNC REPLICA`).
- **Returns the report as a one-row result set** (columns = report fields). Precedent for
  result-returning SYSTEM commands: `SYSTEM SYNC FILESYSTEM CACHE`
  (`InterpreterSystemQuery.cpp:657`) and `SYSTEM JEMALLOC FLUSH PROFILE`
  (`InterpreterSystemQuery.cpp:1250`), both via `SourceFromSingleChunk`. Fallback if plumbing turns
  hostile: full report to the server log only — but the result set is the target.

The paired introspection surface already exists: `system.content_addressed_mounts`
(`StorageSystemContentAddressedMounts.cpp`; columns `disk`, `srid`, lease fields, `state ∈ {live,
expired, terminated, fenced, corrupt}`). "List pool members" is a `SELECT` from that table — reads are
system tables, actions are SYSTEM commands; no `LIST MEMBERS` command is added.

### clickhouse-disks {#disks-surface}

New `ca-drop-member <srid>` command in `programs/disks` (sibling of `ca-inspect` / `ca-gc-dryrun` /
`ca-gc-rebuild`), disk selected via the existing `--disk` mechanism, prints the same report as text.
Both facades are thin wrappers over the one core function. This surface works with no live server.

### Report {#report}

`DecommissionReport` fields: `srid`, `namespaces_removed`, `namespaces_already_removed`,
`committed_refs_removed`, `precommits_removed`, `edge_deltas_emitted`, `manifest_debris_removed`,
`staging_objects_removed`, `mountpoint_objects_removed`, `slot_removed` (bool), `warnings[]`
(e.g. "debris drain incomplete — slot kept terminated"). Deliberately NO byte counts: blobs are
shared, real reclaim is decided by subsequent GC folds.

## Safety and races {#safety-races}

- **Gate:** immediate refusal on a live lease; no FORCE. Self-drop refused automatically.
- **Zombie returns DURING erasure:** its renewal permanently fails (`tripMountLost` — the claim bumped
  the epoch and owns the slot); a full re-open sees our live lease ⇒ `LiveDoubleStart` ⇒ refusal on
  its side. It cannot write (write fence).
- **Zombie returns AFTER erasure:** the slot is gone ⇒ its open re-creates the slot as a NEW empty
  member. Its local metadata references `Removed` namespaces ⇒ its tables come up broken locally —
  documented semantics, the same class as a replica returning after `SYSTEM DROP REPLICA`.
- **Two concurrent decommissions:** the second loses the claim ⇒ refusal.
- **Crash/resume:** every step is idempotent; a re-run re-claims the slot, skips `Removed`
  namespaces, re-drains, finishes. The slot is deleted last, so resume authority survives any crash.
- **GC during erasure:** our beat keeps us live (no fence-out); `dropNamespace` edge deltas fold
  normally; no special synchronization with rounds.
- **Data loss is the intended semantics:** if the victim held the ONLY refs to some parts (lagging
  replication, non-replicated tables), those parts die with it. That is what removing a replica
  means. `INV_NO_DANGLE` is unaffected — ref removal is an ordinary transaction; blobs die later via
  folds.

## Observability {#observability}

- `system.content_addressed_log` events: `member_decommission` begin / per-namespace / end, counts in
  `detail` — the audit lives in the pool, survives the client session, visible from any replica.
- Per-phase server-log lines mirroring the report counters.
- `system.content_addressed_mounts` shows the victim's state progression (`fenced`/`terminated` → row
  disappears after slot deletion).

## Testing {#testing}

- **Core gtests** (fake backend): refusal on live lease; full erasure on a fixture with 2-3 namespaces
  including committed refs + stale precommits + manifest debris + staging + roots objects, with every
  report counter asserted; resume after a simulated crash at EACH phase boundary; zombie-after-claim
  (`tripMountLost` on renewal, `LiveDoubleStart` on re-open); two-command race; fail-close drain
  (backend error ⇒ slot kept terminated + warning).
- **Integration test** (`with_rustfs`, 2 replicas): kill one, run
  `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` from the survivor; assert the survivor is unaffected,
  the victim's namespaces are `Removed`, `ca-fsck` is clean after GC rounds (0 dangling,
  0 unaccounted), the victim's row left `system.content_addressed_mounts`, and the result set carried
  non-zero counters.
- **Stateless test**: grammar parse + privilege refusal without the grant (execution itself belongs to
  the CA lane).
- **ca-soak scenario card** (separate, post-landing): decommission under load + a chaos variant (kill
  the command mid-run, resume).

## Non-goals {#non-goals}

- **Roster:** the durable roster (Part IV, deferred) does not exist and is not built here. Forward
  hook: when it lands, this command additionally removes the member's roster entry.
- **No auto-decommission for inactivity** — deliberate operator action only (the B200 principle
  "never auto-removes for inactivity; a long-absent member pins the floor" stands).
- **Not a replication-layer operation:** ReplicatedMergeTree ZooKeeper metadata is untouched; for
  replicated tables the operator separately runs the ordinary `SYSTEM DROP REPLICA`. Orthogonal
  layers.
- No FORCE variant, no per-namespace granularity, no dry-run, no compatibility scaffolding
  (pre-release standing rule).

## Documentation updates (with implementation) {#doc-updates}

- `docs/superpowers/cas/04-gc-protocol.md`: the offline-replica story gains its ending (decommission).
- `docs/superpowers/cas/05-formats-and-backend.md`: B200 section — implemented without roster,
  grammar renamed to the `SYSTEM CONTENT ADDRESSED …` family.
- `docs/superpowers/cas/ROADMAP.md` + backlog: B200 status update.
- SQL reference (`docs/en/sql-reference/statements/system.md`) — with eventual upstreaming.
