---
description: 'Design for a cross-node snapshot-isolated read-only replica over a shared CAS pool: a reader mount serves SELECTs against a pinned ref-table snapshot, holds a GC retention pin via DataPart-lifetime tokens, and reuses the existing readonly-refresh MergeTree feature with a minimal fork surface'
sidebar_label: 'CAS Read-Only Replica (snapshot pin)'
sidebar_position: 20260714
slug: /superpowers/specs/cas-readonly-replica-snapshot-pin-design
title: 'CAS Read-Only Replica — Snapshot-Isolated Reads with a GC Retention Pin'
doc_type: 'reference'
---

# CAS read-only replica — snapshot-isolated reads with a GC retention pin {#cas-ro-replica}

**Date:** 2026-07-14
**Branch:** `cas-gc-rebuild`
**Status:** design (brainstormed + section-by-section discussed with user 2026-07-13/14)
**Depends on / interacts with:** the ref snapshot+log model
([`2026-07-11-cas-ref-table-snapshot-log-design.md`](2026-07-11-cas-ref-table-snapshot-log-design.md),
amended by the [rev.6 lease-exclusivity proposal](2026-07-13-cas-ref-lease-exclusivity-rev6-proposal.md)),
the pool-member decommission design
([`2026-07-13-cas-pool-member-decommission-design.md`](2026-07-13-cas-pool-member-decommission-design.md)),
and the upstream readonly-refresh MergeTree feature (`refresh_parts_interval` / `MergeTreeData::refreshDataParts`).

## Motivation {#motivation}

Today a CAS pool has exactly one relationship with a table's data: the **writer** mounts a disk
read-write, writes/merges/drops parts, and its own MergeTree part lifetime guarantees it never reads a
part it deleted. There is a pure-observe `read_only` mount (used by the `ca_ro` fsck/introspection disk)
that reads but writes nothing and holds no lease.

We want a **second node to mount the same pool and serve SELECTs** — read-scaling over shared object
storage without full ReplicatedMergeTree/Keeper coordination. The naive form is unsafe precisely because
**CAS has no per-reader read locks**: a blob is kept alive only by its `in-degree` (a live ref references
it), and the ref is kept alive only by the writer's MergeTree DataPart lifetime *on the writer node*. A
read-only second node holds no ref and no lock, so:

> reader lists the directory (part P present) → writer drops P → GC sees P's blobs at `in-degree 0` and
> reclaims them → reader, mid-read of P, gets a `FILE_DOESNT_EXIST` / torn multi-file read.

The reader's "I saw it in the listing" is not durable state anyone respects. This design makes the reader
a first-class, cheap participant in the pool's liveness protocol so its in-flight reads are safe, while
keeping the contact surface with upstream MergeTree minimal (the feature is fork-maintained).

## Decisions (from the design discussion) {#decisions}

| Question | Decision |
|---|---|
| Reader model | **Snapshot-isolated read replica** that writes ONE small durable pin object (r/o to the writer's *data*, writes only its own pin). Not a zero-write observer. |
| Consistency contract | **A: consistent part-data + best-effort table-level.** Snapshot-isolation covers blobs + manifests + ref-table (incl. inline mutable per-part files). The verbatim table-level plane (`roots/<ns>/_files/*`) is best-effort — audited safe (see §Contract A). |
| Retention policy | **Block indefinitely, bounded by the query's own duration** (`max_execution_time` et al.) + reader-lease TTL as the crash backstop. **No `H_max` cap, no "snapshot too old".** |
| Pin precision | **Precise, by real holds** (not a time window): each in-use `DataPart` holds an opaque snapshot-pin token for its lifetime; `S_min = min` over live tokens. |
| Freshness | Reader reads `replay(base_snapshot, tail)` — freshness is the log-append latency, not the snapshot cadence. |
| Refresh | **Reuse the existing readonly-refresh** (`refresh_parts_interval` / `refreshDataParts`), re-keyed on `_snap`. Zero new MergeTree-core code for refresh. |
| MergeTree surface | **Minimal, disk-gated**: one optional pin token on `IDataPartStorage`, plus (verify) a tiny guard so a writer-only artifact does not fail table-open on a readonly disk. Everything else is in the fork's CAS metadata-storage layer. |
| Promotion (reader → writer failover) | **Non-goal here** — feasible on existing primitives (S13 takeover / relink + the decommission fence), promotion-friendly by construction; its own future spec. |

## Architecture overview {#architecture}

Three cooperating pieces, all but one token entirely inside the CAS layer:

1. **Reader mount mode** (new, CAS-side) — a third `Store::open` mode: read-only to data, but holds a
   heartbeated reader-lease and publishes a durable pin. Reuses the `MountLeaseKeeper` heartbeat infra.
2. **Snapshot-isolated read view** (CAS-side + reused MergeTree readonly-refresh) — the reader serves
   `iterateDirectory`/reads from an adopted `_snap` + replayed log tail; a running query holds its
   `DataPart`s (existing MergeTree behaviour); the refresh loop advances the adopted snapshot.
3. **GC retention pin** (one small MergeTree touch + CAS-side GC changes) — each in-use `DataPart`
   carries an opaque snapshot-pin token; the CAS reader derives `S_min` from live tokens and publishes
   it; the writer's GC floors all reclaim at `min` over every reader's pin.

## 1. Reader mount mode {#reader-mode}

CAS currently has two mount modes: **writer** (read-write; anchors the mount-lease keeper; write-fence;
capability probe) and **pure-observe** `read_only` (reads; writes nothing; anchors no keeper; skips the
probe — `Store::open` today: "a read-only open never anchored the keeper"). The read replica needs a
**third mode, `reader`**: read-only to the *writer's data*, but it heartbeats a reader-lease and
publishes a pin.

- `Store::open` gains a `reader` variant. Like `read_only` it performs no writes under the writer's
  `server_root_id` subtree and rejects `createTransaction()` with `READONLY`
  (`ContentAddressedMetadataStorage.cpp:536`) — so INSERT/MERGE/ALTER cannot touch the writer's tree.
- Unlike `read_only`, it starts a lightweight **reader-lease keeper** reusing `MountLeaseKeeper`'s
  heartbeat/renew machinery, but the slot is the pool-global reader-pin object (§4), NOT
  `gc/server-roots/<srid>/mount` and NOT a writer mount-slot (no `writer_epoch`, no write-fence).
- **Invariant (split-brain safety):** the reader writes to exactly ONE key family — its own
  `gc/readers/<reader_id>` pin — and never to anything under the writer's `cas/refs|manifests|roots/<srid>`.
  This is what makes "read-only to the writer's data" real.

Config: the CAS disk is declared read-only for MergeTree (`<readonly>1</readonly>`, so the table is
all-readonly and the refresh path applies) with a CAS `reader` attribute that selects this mount mode.
The reader points at the writer's `server_root_id` as its read source.

## 2. Snapshot-isolated read view {#read-view}

### 2.1 Read `replay(snapshot, tail)`, not the bare snapshot {#replay-tail}

Snapshots are a periodic compaction of the append-only ref log (writer publishes `_snap/<id>` after a
count/bytes trigger). The log is appended **per transaction, immediately**. So the reader must read the
**folded state `replay(base_snapshot, log tail)`** — the same `RefTableState = replay(snapshot, tail)`
state machine the writer, recovery, fsck and GC-intake already share — NOT the bare newest snapshot.
Consequence: **read freshness equals the log-append latency, independent of snapshot cadence** (a slowly
changing table is as fresh as its newest durable log txn, not its last `_snap`).

### 2.2 Snapshot-sourced enumeration via the reused readonly-refresh {#refresh}

Part discovery must come from the adopted snapshot state, never a live `LIST` (else the listing→open
TOCTOU returns). This falls out of the existing upstream readonly-refresh with zero new MergeTree code:

- The table runs with `all_disks_are_readonly && refresh_parts_interval > 0`, so
  `MergeTreeData::refreshDataParts` (`MergeTreeData.cpp:2622`) periodically re-enumerates parts via
  `disk->iterateDirectory` (`:2327/2367`) and diffs against the known set (new parts adopted, gone parts
  scheduled for cleanup).
- The CAS `reader` disk serves `iterateDirectory`/`listDirectory`/reads **from its currently adopted
  `replay(base_snapshot, tail)`**, not the writer's live state. On each `refreshDataParts` tick the CAS
  reader adopts the newest durable snapshot + tail and returns the corresponding part set. Per-part
  identity presented to the diff is the stable manifest id, so the refresh's add/remove logic works.
- **Per-query snapshot isolation is existing MergeTree behaviour:** a running query snapshots the active
  part set at start and holds those `DataPart`s (refcount) for its whole duration; a concurrent refresh
  updates the set for *new* queries only and cannot yank a running query's parts.

## 3. GC retention pin via DataPart-lifetime tokens {#pin}

The correctness requirement: **a manifest/blob a running query might still read must not be reclaimed.**
The natural, precise "how long is the reader using this part" signal is the `DataPart`'s own lifetime
(the query holds it). We pin off that, not a time window (a paused/slow query outlives any time window →
torn read; time cannot know real liveness).

### 3.1 The token (the one small MergeTree touch) {#token}

`IDataPartStorage` gains an optional opaque `snapshot_pin` handle (`std::shared_ptr<void>`), set at
part-storage construction from the disk and released in the storage destructor. A `DataPart` owns its
`IDataPartStorage` for its whole lifetime, so it holds the token for its whole lifetime. Non-CAS / non-
`reader` disks provide `nullptr` (no-op). This is the ENTIRE MergeTree-core change: one nullable member +
acquire-at-construct / release-at-destruct, disk-gated. No query-execution change, no new lifecycle.

The CAS `reader` disk, when it constructs a part-storage for a part resolved in snapshot `S`, hands out a
token registered in a CAS-side live-token registry carrying that part's snapshot. The reader derives
`S_min = min` over live tokens' snapshots and publishes it (§4).

### 3.2 Self-limiting to the race window {#self-limit}

The pin only **binds** on blobs the writer has already dropped (`in-degree 0` in the writer's current
state). For a still-current part the writer's own committed refs keep `in-degree > 0`, so GC never
condemns those blobs and the reader token is redundant-but-harmless there. A part the writer dropped but
a reader still reads is a part refresh removed from the active set yet a **query** still holds — it lives
exactly the query's duration. So the load-bearing pin is query-scoped by construction.

### 3.3 Advanceable tokens (avoid over-retention from long-lived current parts) {#advanceable}

A single `S_min` scalar would be dragged back by a long-lived *current* part loaded at an old snapshot,
over-retaining unrelated churn. Fix: tokens are **advanceable** — on each refresh the CAS reader advances
the tokens of parts still present in the new snapshot to that snapshot; a dropped-but-held part keeps its
token at its last valid snapshot. Then `S_min = the snapshot of the oldest dropped-but-held part` =
exactly what must be retained. This is CAS-side (the reader manages its own token snapshots); the
MergeTree touch stays "DataPart holds an opaque token".

## 4. Durable reader pin object {#durable-pin}

The reader's only durable pool-owned state is one small object:

```
gc/readers/<reader_id>  =  { per-namespace { base_snapshot S_min, tail_head L_max }, lease_expiry }
```

- **Pool-global** (where the blob-reclaiming GC scans), not under any writer `srid`. The reader is NOT a
  pool member (no writable subtree, no `writer_epoch`); it holds a pin + lease only.
- **Heartbeated** (reused keeper cadence); **self-healing** (re-published at start); **crash-safe** (a
  dead reader's pin expires after the lease TTL → GC resumes).
- In-memory (re-derived from the pool on restart, never persisted locally): the per-namespace
  `replay(state)`, per-query pinned view, the live-token registry, and the local part list.

`L_max` is the newest adopted log the reader has folded; `S_min` from §3.3. GC needs the pair to know the
window of ref state the reader's live parts came from.

## 5. GC integration {#gc}

### 5.1 The retention floor gates ALL reclaim paths {#floor}

The writer's GC computes a per-namespace **retention floor = min over every reader pin's `S_min`** (LIST
`gc/readers/`, take the min; expired-lease pins ignored; O(readers), cheap). The floor gates **every**
path that could reclaim something a reader pins — not only blob deletion:

- **Blob condemn/graduate/delete** (source-edge `in-degree`): a blob referenced by any state in a pinned
  window is not condemned.
- **`cleanupRefObjects`** (`CasGc.cpp:1385`): keep `_snap`/`_log` down to the oldest pinned `S_min`, not
  just "covered by the newest snapshot".
- **GC namespace-cleanup** (`CasGc.cpp:1102`, committed manifest bodies + verbatim files): reclaim only
  above the floor.

### 5.2 Condemn-time incremental removal-deferral {#deferral}

GC does not re-fold the whole pinned closure each round (that would be O(window)). Because `in-degree` is
already a *set of source edges*, the floor is applied as **incremental removal-deferral**: a `-1` edge
whose corresponding `+1` lies inside a reader-pinned window is deferred (kept sorted by add-position and
applied as a prefix once `S_min` advances past it). Per-round cost is O(removals newly applicable), not
O(window). A blob is condemnable iff its edge set is empty across `current refs ∪ all pinned windows`.

This overlay is the **shared retention primitive**: a pin contributes an edge-set (a snapshot-window here,
an explicit manifest-closure for a fetch pin) with a liveness owner, and dead-owner pins are dropped
before the fold. The reader pin is one consumer; the fetch-handoff pin
([`2026-07-15-cas-fetch-handoff-retention-pin-design.md`](2026-07-15-cas-fetch-handoff-retention-pin-design.md))
is an independent second consumer built on the same overlay.

### 5.3 Writer snapshot age-trigger (amendment) {#age-trigger}

So a slow table's newest snapshot does not stay ancient (leaving readers nothing recent to re-base onto
and blocking log/snapshot trim), the writer's snapshot-publication trigger gains **age** alongside
count/bytes: publish if the oldest uncovered log is older than `T`. Idle tables (no uncovered logs)
publish nothing; slowly-changing tables get a small periodic snapshot (their state is small). This is an
amendment to the rev.6 snapshot-publication policy and also bounds next-mount recovery time.

## 6. Retention policy — bounded by query duration {#retention-policy}

**Block indefinitely; the bound is the query's own duration.** A running query holds its `DataPart`s
(and thus their pin tokens) for at most `max_execution_time` (and the other query limits); on crash a
reader's pin expires after the lease TTL. So worst-case retention =
`max(oldest active query duration, lease_TTL) × write_rate`, entirely governed by **existing** query/lease
settings — no new `H_max` cap and no "snapshot too old" failure mode. The operator regulates pool
pressure by tuning query timeouts on the reader.

## 7. Many readers {#many-readers}

Read-scaling over one pool is the goal and composes well: readers are independent (no reader↔reader
coordination; each reads shared blobs from object storage in parallel), each owns its own pin key (no
write contention). The only shared resource is the retention floor = `min` over all reader pins. Honest
consequence: **one long-running query on any reader holds the global vacuum floor for the whole pool**;
many readers raise the chance someone holds it. Bounded by `max_execution_time` per §6. Mitigation is
observability, not a mechanism: a `system.content_addressed_*` gauge/column for the oldest reader-pin age
and an alert (the "long read blocks vacuum" signal, exactly a DB long-transaction-blocks-vacuum warning).

## 8. Interaction with namespace drop and pool-member decommission {#drop-interaction}

Both DROP TABLE and `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` reduce to the same floor:

- **DROP TABLE** is ordinary `dropNamespace` → `Removed` snapshot → physical reclaim by normal, now
  reader-pin-aware GC (§5). An active query pinned to `(S, L)` *below* the `Removed` snapshot is
  **unaffected** (it reads its pinned historical closure, retained by the floor); new queries — the
  refresh sees `Removed` — fail "table dropped" (as DROP-under-query does in normal MergeTree). The
  verbatim `format_version.txt` is reclaimed by reader-pin-aware namespace-cleanup for the committed
  closure; a mid-query reader losing a table-level verbatim file after drop is best-effort (contract A).
- **DROP POOL MEMBER** only runs on a **dead** writer (its gate refuses a live lease; no FORCE). It
  erases refs via `dropNamespace` and relies on normal GC folds for physical reclaim ("blobs die later
  via folds"; "command does not wait for blob reclamation"). So the reader-pin floor gates that reclaim
  identically — the reader's pinned closure survives the decommission until the pin releases (≤
  `max_execution_time` + lease TTL); the command itself is not blocked (the reader holds a retention
  floor, not a mount slot). **Integration invariant:** the decommission's step-4 direct manifest-debris
  drain must only touch never-committed pre-precommit debris (never a reader-pinnable committed closure);
  committed manifests are reclaimed only via the reader-pin-aware GC namespace-cleanup (step 3). After
  decommission the reader's source is gone → active queries drain on their pins, new queries fail, and
  the operator repoints (replicated: another member) or detaches the reader.

### 8.1 Fetch-by-relink handoff and bulk warm-up — a separate consumer of the retention overlay {#relink-handoff}

The fetch-by-relink **commit-before-release gap** (`#42`/`#43`) and its future bulk-warm-up
generalization are **orthogonal** to this reader/WORM feature. They share **only** the §5.2
retention-overlay primitive (a pool-global pin contributing an edge-set to GC's fold, dead-owner pins
dropped) and differ on every other axis — owner (a receiver **build** vs a reader **mount**), payload
(a manifest **closure** vs a snapshot-window), liveness (the writer's **`min_active`** build-watermark
floor vs the reader's lease TTL), and publisher (the **sender** on the receiver's behalf vs the reader
itself). They are therefore specified separately and are **independently shippable**: the fetch fix needs
only the §5.2 primitive, not the reader-mount mode, the snapshot-window pin, the reader-lease, or the
readonly-refresh reuse.

See [`2026-07-15-cas-fetch-handoff-retention-pin-design.md`](2026-07-15-cas-fetch-handoff-retention-pin-design.md)
— it owns the fetch protocol, the sender-created / receiver-build-owned pin cleaned by the `min_active`
heartbeat floor, the bulk write-replica warm-up extension, the interim option-C status, and the detached
cluster (B66a/B66b).

## 9. Contract A — the verbatim table-level plane {#contract-a}

Snapshot-isolation covers part-data: immutable blobs, immutable manifests, and the ref-table (including
the **inline mutable per-part files** `uuid.txt`/`txn_version.txt`/`metadata_version.txt`, which live in
the committed-row ref payload and are therefore versioned by the snapshot). The verbatim table-level
plane `roots/<ns>/_files/*` is **best-effort**, and an audit (2026-07-14) found this safe for part-data
reads:

- `format_version.txt` — write-once, content-invariant (only re-emitted with identical bytes on
  disk-add, removed only on DROP). Read on table-open. Safe.
- Mutation entries (`mutation_<N>.txt`, `tmp_mutation_<N>.txt`) — written/renamed only by ALTER (DDL),
  removed only by background `clearOldMutations` of *finished* mutations, read only at table-open
  (`loadMutations`), never on the part-DATA read path. Safe.
- Deduplication log (`deduplication_logs/…`) — the only data-path mutator (INSERT
  rewrites/rotates/deletes it), but **opt-in and off by default** (`non_replicated_deduplication_window
  > 0`), **writer-only** (loaded once at table-open to seed the in-memory map; never read while serving a
  SELECT), and on a read-only disk its `MergeTreeDeduplicationLog::load()` itself calls
  `disk->writeFile` (`:151`) which a read-only CA disk rejects with `READONLY` (`:536`). **Reader
  handling:** do not construct the writer-only dedup log on a `reader` mount (see §10 verify-item).

DDL under a live query (schema change) is MergeTree's existing metadata-snapshot concern, not CAS's.

## 10. MergeTree contact surface (fork-minimal) {#surface}

The whole reader is in the fork's CAS metadata-storage layer (Ring 0) except:

1. **Reuse the existing readonly-refresh** — `refresh_parts_interval` + `MergeTreeData::refreshDataParts`
   (already on the branch, upstream feature). Zero new CAS-specific MergeTree code.
2. **One optional `IDataPartStorage::snapshot_pin`** opaque handle (§3.1), disk-gated (`nullptr` for
   everyone else). The single deliberate MergeTree-core touch.
3. **(Verify) do not fail table-open on a `reader` mount when the writer-only dedup log cannot load.**
   Likely the readonly path already skips constructing it; if not, a tiny guard.

Everything else — the `reader` mount mode, snapshot adoption + `replay(snapshot, tail)`, snapshot-sourced
`iterateDirectory`, the live-token registry + `S_min`, the durable pin + heartbeat/lease, the GC floor +
condemn deferral + `cleanupRefObjects`/namespace-cleanup gating, the writer age-trigger — is Ring 0.

Verify-items before implementation: (a) the dedup-log-on-readonly table-open path; (b) that the
readonly-refresh `iterateDirectory` diff is cleanly drivable by the CAS snapshot semantics (stable
per-part manifest-id identity across refreshes); (c) that `IDataPartStorage`/`DataPartStorageOnDiskFull`
construction is the right single hook for the token across full and projection part storages.

## 11. TLA+ gate {#tla}

Model `CaReadPinCore` (or an extension of the ref-table model). Invariant:

> A blob, manifest, `_snap`, or `_log` in the closure of `replay(S, tail ≤ L)` for any durably-published,
> lease-live reader pin `(S, L)` is neither condemned nor pruned while that pin is live.

Sabotages (each must break the invariant it targets): pin publication lags the read (reader reads a
snapshot below the floor GC has observed) → `INV_NO_DANGLE` violated; lease expiry does not drop the pin
→ reclaim blocked forever (liveness). Witness/liveness: a pin that releases (query ends / lease expires)
is eventually followed by reclaim of everything it uniquely held (`pin_released ~> reclaimed`, no-fairness
lasso check). Model the reader as an observer that pins a `(S, L)` within the durable log and can hold it
arbitrarily long, and GC as folding `current ∪ pinned` edges with incremental removal-deferral.

## 12. Testing {#testing}

- **Core gtests** (fake backend, CAS-side): `S_min` = min over live tokens; advanceable-token behaviour
  (current part advances, dropped-but-held stays); GC condemn-deferral keeps a blob referenced only by a
  pinned window and reclaims it after the pin releases; `cleanupRefObjects`/namespace-cleanup honour the
  floor; multi-reader min-floor; expired-lease pin ignored. All deterministic, no sleeps.
- **DataPart-token gtest** (MergeTree-side, minimal): a `reader`-disk part-storage acquires a non-null
  token held for the `DataPart` lifetime and released on destruction; a non-CAS disk yields `nullptr`.
- **Integration** (`with_rustfs`, 2 nodes): writer churns (insert/merge/drop) while a `reader` node runs
  long + short SELECTs with `refresh_parts_interval`; assert no torn reads / `FILE_DOESNT_EXIST`, reads
  are snapshot-consistent, GC reclaims after queries drain, and the reader picks up new parts (freshness).
  Plus: DROP TABLE under an active reader query; DROP POOL MEMBER of a dead writer under an active reader
  query (query completes, then reclaim proceeds); dedup-enabled table opens read-only without failing.
- **TLA+**: the §11 gate green with its sabotages red before implementation.

## 13. Non-goals {#non-goals}

- **Reader → writer promotion / failover** — feasible on existing primitives (S13
  `claimMountAwaitingExpiry` takeover of the dead writer's `srid`, or fetch-by-relink into the reader's
  own `srid`, both behind the decommission-style dead-writer fence) and this design is promotion-friendly
  by construction (the reader already holds the full replayed state + a fence-aware pin). Its own future
  spec.
- **`H_max` retention cap / "snapshot too old"** — rejected; retention is bounded by query duration.
- **Contract B** (folding the verbatim table-level plane into the ref-table for strict point-in-time on
  table-level files) — the audit showed it unnecessary for part-data.
- **Per-blob read pins** — the DataPart-lifetime token at part granularity suffices.
- **Making the reader a full pool member** — it holds a pin + lease only, not a writable subtree.

## 14. Documentation updates (with implementation) {#doc-updates}

- `docs/superpowers/cas/09-read-protocol.md`: the cross-node reader story (R1/X1 in the backlog gains its
  answer — the ephemeral reader pin, now concrete).
- `docs/superpowers/cas/04-gc-protocol.md`: the retention floor now includes reader pins; condemn
  deferral within pinned windows.
- `docs/superpowers/cas/ROADMAP.md` + `BACKLOG.md`: R1/X1 moves from DESIRABLE/VERIFY to in-progress;
  cross-reference the promotion follow-up.
- `docs/superpowers/specs/2026-07-13-cas-ref-lease-exclusivity-rev6-proposal.md`: note the snapshot
  age-trigger amendment.
