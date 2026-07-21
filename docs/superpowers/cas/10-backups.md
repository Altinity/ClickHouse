---
description: 'Backup and disaster-recovery options for content-addressed (CAS) MergeTree pools: what the object model rules out and what it enables, a comparative survey with RPO/RTO and threat coverage, and the chosen snapshot / mirror / fetch / restore design.'
sidebar_label: 'CAS Backups'
sidebar_position: 10
slug: /superpowers/cas/backups
title: 'CAS MergeTree — Backups and Disaster Recovery'
doc_type: 'reference'
---

# CAS MergeTree — Backups and Disaster Recovery {#cas-backups}

**Status: design survey + chosen direction (2026-07-14). Nothing below is implemented except the
building blocks explicitly marked as existing (`FREEZE` shadow refs, native `BACKUP` read side,
`fsck`/`ca-gc-rebuild`, readonly disk mode). This document feeds and supersedes the bare
"backup/restore runbook" backlog entry (B198) and the AD-3 day-2 runbook item.**

---

## 1. What the CAS model rules out — and what it gives {#constraints-and-assets}

### 1.1 Ruled out: the entire cloud-native versioning stack {#no-versioning-stack}

Bucket versioning must be **off** on the CA prefix (startup probe, fail-closed;
versioning-*suspended* buckets are rejected too — see `01-architecture.md §backend-contract`).
This transitively rules out every backup facility the clouds build on top of versioning:

| Facility | Why unavailable |
|----------|-----------------|
| S3 bucket versioning as an undo window | probe rejects the bucket |
| S3 Replication (CRR/SRR) | requires versioning on both source and destination |
| AWS Backup for S3 (incl. continuous/PITR) | requires versioning |
| S3 Object Lock (ransomware protection) | requires versioning |
| Azure soft-delete / blob versioning / point-in-time restore | must be off for the CA container |

The prohibition is **not** primarily about version bloat. After the ref log+snapshot redesign
(`_log`/`_snap` objects are new keys, mutable churn is tiny: `gc/state`, `mount`, `gc/hb`,
blob `.meta`, `epoch`), the remaining contraindications are semantic:

1. **Deletes stop reclaiming.** On a versioned bucket a `DELETE` (without `versionId`) only
   stacks a delete marker; `deleteExact` would keep the logical namespace tidy while physical
   storage grows forever. Real reclaim = permanent delete of a specific `versionId` — which is
   exactly the deferred "versioned mode (token = `versionId`)" feature.
2. **The safety argument is proved over a different state machine.** Every probe, backend
   binding, and TLA+ model assumes one-live-object semantics. Versioned buckets (marker stacks,
   `If-None-Match:*` over markers, `If-Match` `DELETE` that the matched version survives) are
   unmodeled, and each backend implements them differently.
3. **One-way door.** Versioning cannot be disabled once enabled, only suspended — and suspended
   is rejected even harder. There is no safe "let's try it" experiment on a production pool.
4. **Versioning alone is not a backup.** Mass marker-removal or copy-forward of old versions
   behind GC's back violates the resurrect invariant (never revive a condemned object from its
   stored body); any point-in-time restore built on versions still needs CAS-aware
   `fsck` + `ca-gc-rebuild` tooling.

**Future versioned mode** (token = `versionId`) would re-enable this whole stack and is
*stronger* than the current design (deleting an observed `versionId` has no TOCTOU window; every
`PUT` gets a unique token even for byte-identical bodies, which today requires the fresh
`incarnation_tag` in the object body). The backup story is the strongest argument for eventually
building it. Until then, backup must be built at a different layer.

For reference, what point-in-time restore looks like on versioned buckets elsewhere: AWS has no
in-place bucket rollback (AWS Backup continuous mode gives restore-by-copy within a 35-day
window; DIY via `ListObjectVersions` + copy-forward otherwise); GCS has none (soft delete +
`objects.bulkRestore` covers deletions only); Azure Blob is the only backend with a native
in-place point-in-time restore for block blobs. A PITR cut of a CAS pool would actually be a
*good* restore source — it is equivalent to a crash-consistent state, which the pool tolerates
by design — but all of it sits behind the versioning door.

### 1.2 Assets: what makes CAS unusually backup-friendly {#backup-assets}

1. **Immutability / new-keys-only writes.** Blobs, part manifests, `_log` entries, `_snap`
   snapshots, GC generation artifacts — all immutable, all new keys. The mutable set is tiny and
   enumerable. Incremental object-level copying is therefore trivial: an increment is "the new
   keys", with no rewrite churn.
2. **`gc/*` is entirely rebuildable** (`SYSTEM CONTENT ADDRESSED GC REBUILD`,
   `clickhouse-disks ca-gc-rebuild` — `08-testing-and-soak.md §gc-rebuild-runbook`). The backup
   scope is only `blobs/ + cas/ + roots/ + _pool_meta`; GC state need never be copied.
3. **The pool is self-describing** and survives crash-consistent cuts: restore = mount →
   `fsck` → `ca-gc-rebuild` → drop dangling refs. This is the same machinery as ordinary crash
   recovery.
4. **`FREEZE` already provides in-pool pins**: per-part refs under `shadow/<name>/…` namespaces
   are reachability roots GC respects (verified by scenario S18). A pinned closure cannot be
   reclaimed, which turns `FREEZE` into a consistency anchor for any external copy.
5. **The ref `_log` is a ready-made change feed** — the same deltas GC folds can drive a
   replicator.

One standing caveat for *non-atomic* copies (plain bucket sync): the
`gc/server-roots/<id>/epoch` object is a durable-monotone counter
(`WriterEpochMonotoneUnique`). A sync that captures `epoch` earlier than manifests minted under
a later epoch produces a copy from which a restored writer could re-issue a used
`writer_epoch`. Mitigation: copy identity objects last, or (better) never copy them at all and
let restore re-derive the floor from the `cas/manifests/<ns>/<writer_epoch>/…` key space. The
chosen design (§5) avoids the problem entirely by never replicating identity objects.

---

## 2. Threat model {#threat-model}

| # | Threat | Notes |
|---|--------|-------|
| T1 | Bucket / region loss | infrastructure failure, account deletion |
| T2 | Operator error | `DROP TABLE`, bad mutation, wrong `rm` on the pool |
| T3 | CAS-layer bug | notably a GC bug deleting live data |
| T4 | Credential compromise | attacker with production creds deletes everything reachable |
| T5 | Format dependence | what the restore path requires: a CAS-capable ClickHouse binary within the format's compatibility roster, any ClickHouse, or no ClickHouse at all |

T5 is a structural property, not a statement about format instability: the pool format evolves
under the documented schema-evolution rules (`05-formats-and-backend.md §schema-evolution`:
self-describing format version, write-down-to-floor, a supported version roster), so a
CAS-format backup is restorable by any binary within the roster. The tiers differ in what the
restore path *requires*: a CAS-capable ClickHouse (CAS-format copies), any ClickHouse onto any
disk type (plain MergeTree files, C1), or no ClickHouse at all (open formats, E). For
long-retention chains, record the pool format version with the chain (the pool is
self-describing via `_pool_meta`) so the restore runbook can pick a compatible binary.

---

## 3. Survey of options {#options-survey}

### 3.1 Infrastructure level {#infra-level}

**A1. Versioning / Object Lock / CRR / AWS Backup — unavailable.** See §1.1. Must be stated
explicitly in the runbook, because it is the first thing an SRE will reach for.

**A2. Periodic bucket sync (`rclone` / `aws s3 sync` / `s5cmd`) to another bucket/region.**
Works thanks to immutability. Two races to codify: the GC race (a ref copied before the blobs of
its closure are copied, while GC reclaims them mid-window — mitigated by `FREEZE`-pinning the
copied set, pausing GC, or a second fixpoint pass) and the epoch race (§1.2). Restore = point
the disk config at the copy → `fsck` → `ca-gc-rebuild` → drop dangling refs. RPO = sync period;
RTO = minutes–hour. Cost: LIST-heavy on large pools; deletes never propagate, so the copy needs
its own trimming or periodic re-baselining.

**A3. Event-driven mirroring** (`ObjectCreated` events → copy; deletes deliberately not
mirrored). RPO ≈ minutes; no LIST cost; but S3 events are not delivery-guaranteed, so a
reconcile sync (A2) is still required as a safety net, plus retention machinery on the
destination. Own infrastructure (queue, DLQ, monitoring).

**A4. Storage-layer snapshots (self-hosted backends only).** RustFS on ZFS/btrfs, EBS
snapshots: instant crash-consistent point-in-time, COW retention nearly free, restore =
rollback/clone + the standard crash-recovery path. Not applicable to real S3 (no bucket-snapshot
primitive); snapshots co-located with the storage cluster unless shipped out.

### 3.2 In-pool level ("prevent real deletion") {#in-pool-level}

**B1. `FREEZE` shadow refs (exists today).** `ALTER TABLE … FREEZE` publishes one ref per
frozen part under `shadow/<name>/…`; zero bytes move; storage cost = pinned churn only
(unchanged parts are shared with the live table by hash). Instant snapshot, instant restore
(ref republication). Protects against T2 only: same bucket, same format, same failure surface —
a GC bug that deletes live blobs kills the pins with them. Operational note: `SYSTEM UNFREEZE`
is disabled in the default config (S18).

**B2. Retention-delayed GC graduation (small proposed feature).** GC already has two-phase
condemn → graduate; adding a "graduate no earlier than R after condemn" knob makes every state
within R recoverable — the `_log` history plus still-present blobs give in-pool point-in-time
restore for free (replay `_snap`+`_log` to T; condemned-blob resurrection already exists). A
trash-prefix variant (copy+delete) was considered and is worse: S3 has no rename, so it doubles
writes; pure graduation delay costs zero extra operations. Storage cost = churn × R. Same
threat coverage as B1.

### 3.3 Native `BACKUP`/`RESTORE` {#native-backup}

**C1. `BACKUP TABLE … TO S3(...)` / `TO Disk(...)` — materialized, format-independent.**
The read side works today; `RESTORE` routes each part through a whole-part transaction. `BACKUP`
reads parts through the read path (manifest → blobs → decoded bytes) and writes **plain
MergeTree files** — the only option in this survey whose output does not depend on the CAS
format at all. It therefore covers T3 and T5, which nothing CAS-format-shaped can. Incremental
via `base_backup` chains. Costs: full read + full write through the server per backup
(`allow_s3_native_copy` is inapplicable: a blob object carries the envelope header and possibly
inline/shared placement, so raw `CopyObject` cannot produce file bytes). Note that `BACKUP`
*does* use the freeze mechanism internally — temporary hardlink clones pin the part set for
consistency (`IDataPartStorage::backup` with temporary hardlinks; ref-level clones on CAS) —
but that covers only the snapshot-of-the-moment half of `FREEZE`; the output is still a full
materialization, so none of `FREEZE`'s cheapness transfers.

**The same-CA-disk trap.** `BACKUP … TO Disk(<the same CA disk>, 'backups/…')` lands the output
as **verbatim loose objects** inside the pool prefix (a backup directory is not a table dir, so
the `@cas@` mapping stores it non-content-addressed): no dedup, full byte cost, *and* the copy
lives inside the CAS ownership domain it is supposed to be independent from. A same-bucket
materialized backup should instead target a different prefix via the `S3()` backup engine or a
separate plain s3 disk, bypassing the CAS layer entirely.

**C2. Same bucket, different prefix (via `S3()`).** Covers T2/T3/T5 but not T1/T4. Reasonable as
a cheap middle tier when only one bucket is available.

### 3.4 `clickhouse-backup` (Altinity) {#clickhouse-backup}

The classic mode (freeze + copy local `shadow/` files + upload) is incompatible with CAS: there
are no local metadata files and no filesystem shadow — shadow lives in-pool as refs. Only the
embedded mode (which wraps native `BACKUP`) applies, adding scheduling/retention orchestration
on top of C1. Not an independent option.

### 3.5 Logical export (Iceberg / Parquet / datalake) {#logical-export}

Scheduled `INSERT INTO FUNCTION iceberg(...)`/`s3(...)`, or partition export as data cools.
Ultimate independence (readable without ClickHouse — T5 coverage beyond even C1) and directly
queryable by other engines; but lossy (`AggregateFunction` states do not survive Parquet;
`LowCardinality`, codecs, skip indexes, projections, physical `ORDER BY` layout are lost),
consistency on a moving table is DIY, and restore = full re-insert — the longest RTO in this
survey. An archival tier, not an operational backup.

### 3.6 DR replica on its own pool {#dr-replica}

A `ReplicatedMergeTree` replica in another region with its **own** pool. Fetch-by-relink
applies only within one pool (`pool_uuid` equality — `01-architecture.md §pool-uuid-relink`), so
a foreign-pool replica does honest byte fetches → a continuously maintained independent copy.
RPO ≈ replication lag; RTO ≈ 0. Replicates `DROP TABLE` too, so it covers T1/T4 but not T2 —
combine with B1/B2 on the DR side. Costs 2× storage + cross-region traffic + a second Keeper.

### 3.7 CAS-native backup — the chosen direction {#cas-native-option}

Backup = a second CAS pool (different bucket/region/credentials) that receives **snapshot
ref-sets + their closures by hash**: only blobs absent on the destination are copied
(server-side `CopyObject` within a region), the result is a real pool verifiable by `fsck`.
Incremental by construction, dedup-preserving, cheap. Shares the CAS format (T5: restore goes
through a CAS-capable binary, see §2). Detailed in §5.

---

## 4. Comparison matrix {#comparison-matrix}

| Option | T1 bucket loss | T2 operator error | T3 CAS bug | T4 creds | T5 format independence | RPO | RTO | Cost |
|---|---|---|---|---|---|---|---|---|
| A1 versioning stack | — | — | — | — | — | **unavailable** (probe fail-closed) | | |
| A2 periodic sync | ✅ | ✅ | ⚠️¹ | ✅ | ❌ | hours | minutes–hour | LIST + 2× storage |
| A3 event mirror | ✅ | ✅ | ⚠️¹ | ✅ | ❌ | minutes | minutes–hour | infra + 2× storage |
| A4 storage snapshot (self-hosted) | ⚠️ same cluster | ✅ | ✅ | ⚠️ | ❌ | minutes | minutes | COW, cheap |
| B1 `FREEZE` shadow | ❌ | ✅ | ❌ | ❌ | ❌ | ~0 | ~0 | ~0 |
| B2 retention GC (PITR) | ❌ | ✅ | ❌ | ❌ | ❌ | ~0 within R | minutes–hours | churn × R |
| C1 native `BACKUP` (other bucket) | ✅ | ✅ | ✅ | ✅ | ✅ | hours–days | **hours** (rematerialize) | full materialization |
| C2 native `BACKUP` (same bucket) | ❌ | ✅ | ✅ | ❌ | ✅ | hours–days | hours | full materialization |
| D `clickhouse-backup` embedded | = C1 + orchestration | | | | | | | |
| E Iceberg/Parquet export | ✅ | ✅ | ✅ | ✅ | ✅✅ | hours–days | **longest** (re-insert) | lossy |
| F DR replica (own pool) | ✅ | ❌ | ⚠️² | ⚠️ | ❌ | seconds | ~0 | 2× everything |
| G CAS-native backup (§5) | ✅ | ✅ | ⚠️¹ | ✅ | ❌ | minutes (cold tier) / consolidation cadence (hot tier, §5.3) | minutes | cheap (dedup + server-side copy) |

¹ Object-level copies preserve *past* states across a GC bug, but a bug corrupting object
bodies or the format replicates into the copy.
² Independent bytes, same code — correlated failure.

No single tier covers the matrix. The recommended composition: **B1+B2** (in-pool tier, ~free,
consistency anchor) + **G** (primary DR tier; A2 with a documented runbook is its manual
stand-in until built) + **C1 at a rare cadence** (weekly — the only tier restorable without a
CAS-capable binary). E optionally as a fourth archival tier.

---

## 5. Chosen design: snapshot / mirror / fetch / restore {#chosen-design}

### 5.1 The four verbs {#four-verbs}

`BACKUP`/`RESTORE` are operations **inside one pool**; movement between pools is always the
same primitive — selective replication. Restore is never cross-pool; it is always a local
relink. The model is deliberately git-shaped:

| Verb | Analogy | Cost |
|------|---------|------|
| `BACKUP` — per-disk snapshot | `git tag` | instant, zero bytes, no hashing |
| consolidate — make a chosen snapshot pool-complete (§5.3) | `git push` of node-local objects | async; sets the hot tier's DR RPO |
| mirror — pull daemon, prod → backup pool | `git push --mirror` | continuous, sets RPO |
| fetch — selective pull, backup pool → fresh pool | partial clone | sets RTO part 1 |
| `RESTORE` — in-pool relink | `git checkout` | instant |

One closure-walk + hash-verification primitive serves all three pool-to-pool movements (mirror
out, fetch in, adoption into a live pool); consolidation is the one movement that instead goes
through the ordinary write path — it creates new pool objects rather than copying existing
ones.

### 5.2 In-pool snapshots {#in-pool-snapshots}

**Upstream surface (CAS-independent track).** A `BACKUP` destination engine — working name
`BACKUP TABLE t TO Snapshot('name')` — with the semantics "engine-native snapshot": hardlink
clones on local disks, the CAS binding below on CA disks. Riding the `BACKUP` framework buys,
for free: RBAC (`BACKUP`/`RESTORE` grants are already separate from `ALTER`; snapshot
*deletion* — the operation that destroys restore points — gets its own restricted right,
today's `SYSTEM UNFREEZE` gating being the precedent), `system.backups` introspection plus a
snapshot-listing table, DDL/metadata capture, `ON CLUSTER` coordination.
`RESTORE … FROM Snapshot('name')` = relink/clone-attach; additionally a read-only
`ATTACH`-from-snapshot for instant mounting without restore. Snapshot lifecycle is decoupled
from the table (`DROP TABLE` never removes snapshots). Thinning follows a GFS schedule
(e.g. every 10 min → hourly → daily bands). Scheduling itself can stay external in v1.

**CAS binding: one snapshot object, referencing — never copying — manifests.** After the ref
log+snapshot redesign, "the full table state in one object" already exists: the table's current
`_snap`. A snapshot is a copy of that `_snap` body into a shadow namespace —
`cas/refs/shadow/<name>/<table>@cas@/_snap/<txn>.proto` — one `PUT`, one object, plus a small
metadata object (DDL, schema version, timestamp). It references the same part manifests:

- manifest sharing by multiple refs is first-class (mutable per-part fields live in the
  `RefPayload` precisely so identical parts share one manifest);
- manifest lifetime is edge-driven (the source-edge in-degree set), not namespace-driven — a
  shadow ref's edge protects a manifest across namespaces, which today's `FREEZE` already
  relies on (S18);
- consequently a standing invariant: **manifest cleanup must remain edge-driven forever** — no
  future "wipe the namespace prefix" tooling may bypass the in-degree check, because snapshots
  keep referencing manifests under dropped tables' prefixes.

Storage cost of the snapshot tier = the integral of churn over the retention bands (adjacent
snapshots share unchanged parts for free); merges dominate it, and GFS thinning is what keeps
the long tail affordable.

### 5.3 Multi-disk tables (hot local + cold CAS) {#multi-disk}

`FREEZE`/`BACKUP` already iterate all disks of the storage policy, and the part set is
consistent (each active part lives wholly on one disk; moves are atomic at the active-set
level). The problem is that hot-tier parts do not exist in the pool — the freshest data would
get the weakest protection. Uploading the hot delta on *every* frequent tick was considered
and rejected: it hashes and uploads young merge churn (most hot-part generations die in merges
before ever moving cold), and it contradicts the frequent tier's purpose — instant local
restore.

Chosen resolution: **cadence and completeness are separate axes.**

1. **Frequent snapshots are per-disk native** — the natural `FREEZE` shape: hardlink clones on
   the local disk, a shadow `_snap` on the CA disk — correlated across disks by the snapshot
   name (the `FREEZE WITH NAME` precedent). The snapshot metadata object records which disk
   holds which piece and whether the snapshot is pool-complete, so tooling can always answer
   "restorable from where?". Capture is instant and involves **no hashing at all**. These
   snapshots serve operator-error recovery with instant restore on both tiers; the hot piece's
   durability equals the node's — by design.
2. **Consolidation is a derived, asynchronous operation on a chosen existing snapshot** —
   "gather snapshot X onto the pool": upload the hot pieces' blobs through the ordinary write
   path **from the snapshot's own frozen hardlinks** (so the consistency point stays the
   original capture moment regardless of upload duration), then publish the pool-complete
   shadow `_snap`. Runs on its own band (e.g. daily) with an optional per-table cadence
   override. Only pool-complete snapshots are units for the mirror (§5.4) and for full-table
   fetch/restore (§5.5).

Economics: at consolidation cadence most young churn has died; the surviving parts are mostly
those that will move cold anyway, so their upload is a prepayment — when the TTL move later
happens, the write path dedups against the already-present blobs and the move becomes
metadata-only.

**Consolidation identity is hashing — the CA-native primitive, with no cross-backup
metadata.** The consolidator simply streams every hot file of the chosen snapshot through the
ordinary write path: hash under the pool's blob algorithm, then `putIfAbsent`/cold-reuse — a
blob already present (from a previous consolidation or the trickle warmer) costs a `.meta`
point-read and no upload. Incrementality therefore falls out **pool-side**: the pool is the
only index, and the current backup is never compared against the previous one via side
metadata. Manifest reuse falls out the same way: `ManifestId` is monotone, not
content-derived, so when every file of a part resolves to the same digests as in the previous
consolidated manifest, that `ManifestId` is re-referenced; otherwise a new manifest is minted
over the (mostly deduplicated) blobs. The trickle warmer needs no bookkeeping either: a part
crosses the age threshold exactly once, so the warmer processes the parts that crossed it
since its last pass (cursor = a timestamp; losing it means some re-hashing that the pool
dedups — harmless). Skip-read shortcuts were considered and **rejected**: bare
`checksums.txt` equality (MergeTree checksums are `CityHash128`, a non-cryptographic hash —
it must not gate a decision to skip reading bytes), and inode witnesses / borg-style files
caches keyed by filesystem identity (correct, but cross-backup comparison over filesystem
metadata is side state alien to the CA model, whose identity primitive *is* re-hashing). The
price of this simplicity is one streaming read+hash of the hot tier per consolidation —
minutes per terabyte at a daily band on a local NVMe tier; if that ever becomes a measured
bottleneck, the lever is the pool's pluggable hash speed, not a bypass of hashing. In-pool
blob dedup has its own collision axis, governed by that pluggable choice: deployments that
require collision resistance run the pool on `sha256`, and a slot-bound middle tier
(`ch128ctx` = content hash ∥ `xxh3_64(part_name, file_name)` ∥ size — see
`BACKLOG.md §read-write`) defuses *cross-slot* collisions (the realistic adversarial dedup
vector) at near-zero CPU cost, while every dedup CAS actually relies on survives: relink and
carry-forward are reference-based, and retry idempotency, same-name replica writes, and the
snapshot-upload → TTL-move prepayment are all same-slot.

Optional refinement — the **age-based trickle warmer**: a low-priority background uploader
pushes blobs of hot parts older than a threshold `A` into the pool ahead of any snapshot (a
part that survived the young-merge window will likely reach the cold tier eventually). With
the warmer on, consolidation degrades into publishing metadata, and TTL moves become nearly
free as a side effect; `A` trades doomed-churn upload volume against the consolidation
window's size.

The RPO structure that falls out: operator error — the frequent cadence, both tiers; node
loss — hot data since the last consolidation is gone (it lived only on the node); bucket
loss — frequent cadence for the cold tier, consolidation cadence + mirror lag for the hot
tier. Where the node-loss hot RPO is unacceptable, the lever is a per-table consolidation
cadence — and beyond that, continuous protection of fresh data is the DR replica's job
(§3.6), not the snapshot tier's.

Restore order: everything comes up as refs on the CA disk (instantly queryable, cold); the
storage policy re-warms hot parts in the background; on a surviving node the local hardlink
pieces restore hot parts instantly without any download. Deliberate future extension (out of
v1 scope): consolidation for tables with no CA disk at all (`Snapshot('name',
disk='cas_disk')`) would turn the pool into a deduplicating backup store for arbitrary
MergeTree tables.

### 5.4 Mirror: the pull daemon {#mirror-daemon}

A standalone process on the **backup side**, pulling with read-only production credentials and
writing with its own (production-credential compromise cannot reach the backup — T4). Cycle:

1. `LIST` only `cas/refs/` on the source (hot prefix — the D0 lesson: never a full-pool or
   blob LIST per round) → new snapshot objects since the cursor;
2. walk each snapshot's closure (snap → manifests → blobs), set-difference against the
   **destination's own index**, copy what is missing (server-side copy within a region);
3. never propagate deletions; apply an **independent retention policy** on the destination —
   its timeline is a superset, so mass snapshot deletion on the source (T2/T4) does not cascade
   into the backup.

Replicating *snapshot closures* rather than the live head removes the GC race by construction:
everything the daemon reads is pinned by the snapshot's shadow refs, so no ack-floor/lease
integration with source GC is needed (rejected alternative: a lease-holding live-tail
replicator — RPO→seconds, but it adds a GC-wedging actor of exactly the class P3.1 removed).
The one ordering rule: source-side thinning of a snapshot must happen-after its durable
replication — trivial when one scheduler owns both decisions.

The destination is a **real CAS pool**: the daemon mounts it as an ordinary writer (own
`server_root_id`, epoch, mount lease) and runs the **standard GC** there to reclaim thinned
snapshots — a single-writer pool whose only "tables" are shadow namespaces. No special
"streaming GC" exists; there is no coexistence problem on either side (on the source the daemon
is a pure reader of pinned closures). Verification comes for free: `clickhouse-disks ca-fsck` on
the destination *is* backup verification, and a scheduled rehearsal mode (mount r/o → attach a
snapshot → `CHECK TABLE`/sample queries → report) makes restore testing routine and free.

### 5.5 Recovery: fetch + `RESTORE` {#recovery}

**Invariant `BACKUP-BUCKET-READONLY`: no restore path ever writes to the backup pool.** Not an
identity claim, not GC, not a ref — reads only. A backup you write to is a backup you had one
chance with.

- **Selective fetch.** Recovery pulls a *chosen subset* into a fresh (or living) pool: pick
  snapshot objects (default "latest pool-level volume"; options `--at <timestamp>`,
  `--tables`, bands), compute closures, copy. Ancient backups' unique blobs are simply not in
  the selected closure; fetching several snapshots of one table costs the union of closures,
  not the sum.
- **Local relink `RESTORE`.** After fetch, the fresh pool holds the shadow `_snap` objects,
  manifests, and blobs under their original (foreign) namespace keys. Cross-namespace manifest
  references are first-class (`ManifestId` embeds its `root_namespace`; `FREEZE` relies on this
  today), so `RESTORE` republishes live-table refs pointing at the imported manifests — zero
  bytes, minutes. No identity surgery: identity objects are never replicated; the fresh pool
  mints its own `owner`/`epoch`, and imported manifests cannot collide with new ones because
  the namespace is part of manifest identity. Imported snapshot namespaces may be kept (the
  local history continues) or dropped — edge-driven GC reclaims exactly the unreferenced part.
- **RTO optimization: the backup pool as a readonly restore tier.** When full-fetch RTO is too
  slow: storage policy = fresh writable CA disk + the backup pool mounted as a **readonly**
  disk (readonly mounts claim no lease/epoch — the `fsck`/`ca-gc-dryrun` discipline). Reads
  work within minutes straight off the backup; writes go to the fresh pool from minute one;
  fetch degrades into a background drain by ordinary move machinery, hot partitions first.
- **Last resort (documented as backup-consuming): in-place fail-over.** Mounting the backup
  bucket as the new production pool destroys it as a backup at the first `PUT`. If ever used:
  `fsck` before the first write, GC and thinning disabled until an operator explicitly enables
  them, and a new backup chain to a third bucket starts immediately. The runbook keeps this in
  a red frame and points at the three modes above.

Write-path separation, summarized: production writes only to production; the daemon writes only
to the backup pool; restore writes only to the fresh pool.

### 5.6 Invariants {#backup-invariants}

| Invariant | Statement |
|-----------|-----------|
| `BAK-RO` | No restore path performs any write to the backup pool; the only exception is the explicitly operator-confirmed in-place fail-over, documented as consuming the backup. |
| `BAK-EDGE-DRIVEN-MANIFESTS` | Part-manifest reclamation is decided by in-degree edges only, never by namespace-prefix ownership; snapshots may reference manifests under dropped tables' prefixes indefinitely. |
| `BAK-THIN-AFTER-MIRROR` | A snapshot designated for consolidation or mirroring may be thinned only after that operation durably completed (consolidate before thin; mirror before thin). |
| `BAK-INDEPENDENT-RETENTION` | Destination retention is computed independently; source deletions never cascade. |
| `BAK-NO-IDENTITY-COPY` | `gc/server-roots/*` identity objects are never replicated; every pool mints its own identity. |

### 5.7 Open questions {#open-questions}

1. **Pool-level volume object.** Table snapshots are per-table; "restore everything to 12:40"
   and the daemon want a pool-level unit — a light object listing the table snapshots of one
   tick. Shape and placement TBD.
2. **Mirror scope.** Replicate only pool-complete snapshots (simple: every mirrored snapshot
   restores a full table) vs also the cold-partial frequent snapshots (nearly free — their
   closures are in-pool already — and it lowers the cold tier's external RPO to the frequent
   cadence, at the cost of partial-restore semantics in the runbook). Leaning:
   consolidated-only in v1. Related: the default age threshold `A` for the trickle warmer, if
   built.
3. **Daemon packaging.** `clickhouse-disks ca-backup-pull` subcommand (precedent: `ca-fsck`,
   `ca-gc-dryrun`, `ca-gc-rebuild`, with the same readonly-mount discipline) vs a standalone
   service. v1 leaning: `clickhouse-disks` + cron.
4. **Snapshot metadata format** (DDL capture, mutable per-part payload granularity, versioning)
   — small, but it becomes a compatibility surface between versions.
5. **RBAC details** — dedicated grants for snapshot create/delete; interaction with the
   disabled-by-default `SYSTEM UNFREEZE`.

---

## 6. Relation to existing docs and backlog {#relations}

- `01-architecture.md §freeze-materializes-bytes` — the byte-materialization contract of
  `shadow/` is satisfied through the disk API view (files reconstruct on read); the in-pool
  representation is per-part shadow refs (`05 §path-mapping`, S18).
- `04-gc-protocol.md` — condemn/graduate two-phase tail (B2 hooks into graduation), edge-driven
  in-degree model (`BAK-EDGE-DRIVEN-MANIFESTS` hardens it into a permanent commitment).
- `05-formats-and-backend.md §layout-keys` — `_snap`/`_log` objects the snapshot binding
  copies; the `@cas@` verbatim boundary behind the same-CA-disk `BACKUP` trap.
- `08-testing-and-soak.md` — `ca-fsck` (backup verification), `ca-gc-rebuild` (restore step),
  S18 (shadow reachability).
- `BACKLOG.md` — B198 (this document is its design base), "out-of-band staging adoption via
  verified copy-forward" (the fetch/adoption primitive), B180 format-version breadcrumb (the
  self-description T5's roster rule leans on), AD-3 day-2 runbook (restore procedures belong
  there once implemented).
