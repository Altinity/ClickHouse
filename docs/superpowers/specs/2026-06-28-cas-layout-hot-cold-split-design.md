---
description: Design for an explicit server_root_id-rooted content-addressed pool layout with a hot/cold split (cas/refs vs cas/manifests), a mount ownership+heartbeat startup protocol (single sticky owner object = identity, lease = liveness), a durable-monotone writer_epoch allocated at mount claim, a cursor-paced bounded orphan-manifest sweep, and the manifest_ordinal identity reshape. GC discovery becomes a single LIST over ref shards instead of a recursive walk of roots/.
sidebar_label: CA layout hot/cold split
sidebar_position: 1
slug: /development/cas-layout-hot-cold-split-design
title: CA pool layout — server_root_id identity, hot/cold split, mount safety
doc_type: reference
---

# CA pool layout — `server_root_id` identity, hot/cold split, mount safety {#ca-layout-hot-cold-split}

## Problem {#problem}

Two problems, addressed together because they share the pool layout:

**1. GC discovery is quadratic over the mirrored tree.** The background scheduler runs a round roughly every
2 seconds; each round first reads the per-ref-shard tokens to decide which shards changed since the last fold
(the Phase-2 token-diff: skip a shard whose listed token still equals the sealed `folded_token`). Today ref
shards live at `roots/<namespace>/<shard>`, **interleaved under the same `roots/` subtree** as the per-build
part-manifest backlog (`roots/<ns>/_manifests/…`, tens of thousands of objects) and verbatim mirrored files
(`roots/<ns>/_files/…`). So discovery does a **recursive LIST of all of `roots/`** to read a handful of
ref-shard tokens, paging the whole backlog every round — and `CasObjectStorageBackend::list` passes
`max_keys=0` (unbounded) + client-side paging, so each page re-fetches the whole prefix → O(N²/page). This is
`GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS`.

**2. Layout identity is implicitly derived from `ServerUUID`,** with a "stable unless the uuid file is lost"
footgun: if the local ClickHouse `uuid` file is regenerated the namespace root silently switches, and nothing
prevents two live servers from mounting the same pool path and writing the same namespace tree.

## Goal {#goal}

- Make hot-path discovery cost **`GET gc/registry` + `LIST cas/refs/`**, returning *only* ref-shard objects
  (and tokens) — never manifest bodies, verbatim files, or the mirrored tree.
- Make layout identity **explicit and configured** via a required `server_root_id`; demote `ServerUUID` to a
  runtime owner token. Guard the mount with two composable startup checks: a clock-free **single sticky owner
  object** (identity) and an **active heartbeat lease** (liveness).
- Move the manifest backlog to its own cold prefix, swept by a cursor-paced, budgeted background pass.

Layout + naming + an explicit identity/mount discipline. No content-addressed trees/manifests, no new
discovery index object, no cross-namespace manifest sharing, no registry redesign. `gc/registry` stays the
authority for the namespace universe and the registry fence; `LIST cas/refs/` is only a token accelerator.

## Server-root identity {#server-root-identity}

`content_addressed.server_root_id` is a **required** config parameter. `ServerUUID` no longer participates in
layout identity; it remains only a runtime owner token for the startup self-check and diagnostics.

```xml
<metadata_type>content_addressed</metadata_type>
<server_root_id>{replica}</server_root_id>          <!-- macro-expanded, e.g. shard-01/replica-a -->
```

**Validation + immutability.** `server_root_id` is a key component, so it must be a **clean relative path**:
non-empty; no leading/trailing `/`; no `//`; no `.`/`..` segments; bounded length; and no segment may collide
with the reserved `_files`/`_manifests` names (the existing `checkNamespace` rules, applied to the prefix).
It is **immutable for a given server**: changing it strands the old tree under the old root, and the owner
object (below) makes the old `server_root_id` permanently bound to that `ServerUUID`. The configured value is
validated at disk init; an invalid value fails disk start with a clear message.

Every namespace is rooted at `server_root_id`:
`root_namespace = <server_root_id>/store/<u3>/<table_uuid>@cas@`. Consequences:

- **Per-server metadata is `server_root_id`-scoped** — `cas/refs/<server_root_id>/…`,
  `cas/manifests/<server_root_id>/…`, `roots/<server_root_id>/…`.
- **`blobs/` is NOT rooted** — content-addressed blob bodies (`blobs/<aa>/<hash>`) stay pool-global and shared
  across servers (the "shared blobs, per-server trees" invariant; dedup spans replicas, trees do not).
- Because the namespace already carries server identity, **`writer_epoch` is a per-`server_root_id` value**
  with no server prefix (see [Terms](#terms)).
- No `ServerUUID` fallback, no compatibility branch. CA is pre-release with no persisted data ⇒ **no migration**.

## Mount ownership and heartbeat {#mount-safety}

Two composable startup checks protect a `server_root_id`. **The owner object is identity; the lease is
liveness.** Hard rule: the lease (and its wall-clock TTL) must never decide *whether this is the right
owner* — only *whether an owner is currently live*.

### Single sticky owner object (identity, clock-free) {#owner-marker}

```
gc/server-roots/<server_root_id>/owner        # ONE object; putIfAbsent; holds our server_uuid_hex
```

On writable startup, a single `putIfAbsent` of our `server_uuid_hex` — **no owner set, no LIST** (safety must
not depend on a complete listing):

- **absent** → our `putIfAbsent` wins → we are the owner → continue;
- **present, equals ours** (a `GET` after a lost `putIfAbsent`) → continue;
- **present, differs** (or malformed) → **fail closed**.

The object is **sticky**: never auto-deleted on shutdown. If `ServerUUID` is regenerated (uuid file lost),
startup **fails** instead of silently switching the namespace root. Read-only open does not mutate but may
validate if the object exists. One sticky object (vs an `owners/<uuid>` set + LIST) avoids poisoned
multi-marker states and removes a safety-critical listing — identity reduces to a single `GET`/`putIfAbsent`.

### Active mount heartbeat lease (liveness, same-UUID reclaim only after expiry) {#mount-lease}

```
gc/server-roots/<server_root_id>/mount
```

Body: `server_root_id, server_uuid, writer_epoch, hostname, pid, started_at_ms, seq, expires_at_ms`.

Startup protocol (runs **after** the owner-object gate passes, so the `server_uuid` here is already ours):

- **absent** → `putIfAbsent` our body;
- **same `server_uuid` + lease EXPIRED (or stale/absent fields)** → CAS-overwrite to reclaim: allocate a fresh
  `writer_epoch` ([below](#writer-epoch-alloc)), advance `seq`, set `expires_at_ms = now + ttl`. This is the
  same server restarting after its prior process's lease lapsed;
- **same `server_uuid` + lease NOT expired** → **fail closed** — a *live* lease means another process of this
  same server is mounted (an accidental double-start); wait for expiry. (A restart always carries a different
  `writer_epoch`, so "same uuid + live lease" is never us-twice-legitimately.)
- **different `server_uuid`** → **fail closed, regardless of expiry** (the owner gate already rejected this;
  restated so the lease branch is unambiguous).

**Cross-`ServerUUID` takeover is never automatic** — even an expired lease + foreign UUID fails closed.
Reason: `server_root_id` is an identity/layout boundary, not a transient lease; a wall-clock TTL must not
decide "same owner or new"; auto cross-UUID takeover is unsafe under clock skew, VM pause, partition, or an
old server still alive. Failover to a different physical server is an **operator action**: restore the old
`<clickhouse_path>/uuid`, or explicitly remove the stale owner object after verifying the old server is dead.

Background renewer (a periodic scheduled task — no `sleep`-loop): CAS-overwrite the same object every
`mount_renew_period`, advancing `seq` and `expires_at_ms = now + ttl`. **Renew-failure policy:**

- token mismatch but body is **still ours — same `server_uuid` AND same `writer_epoch`** → bounded retry
  (a transient CAS race with our own renew);
- body has **our `server_uuid` but a different `writer_epoch`** → we have been **superseded** by a newer
  incarnation of this same server (it reclaimed an expired lease) → treat the disk as **lost**: stop accepting
  writes / fail closed. **This is not a retry.**
- body **foreign `server_uuid`** → disk **lost**: stop accepting writes / fail closed;
- backend error → fail closed for writes until recovered, per the existing disk-failure policy.

### Write-path lease fence (paused-process safety) {#write-fence}

An expired-lease reclaim by a new process does **not**, by itself, stop the *old* process from writing — the
old process could resume after a long pause (VM pause, long GC, swap) believing it still holds the mount. To
prevent a superseded writer from corrupting the live one's ref shards / owner log, **every mutable CAS/PUT is
fenced on the local lease**: a write proceeds only while this process's `(server_uuid, writer_epoch)` is the
one currently in the `mount` body **and** the local lease is unexpired. `writer_epoch` is the **fencing
token**: the moment the renewer observes the mount carrying a different `writer_epoch` (or foreign uuid, or an
expired lease it cannot renew), the disk trips to **lost / read-only-fail-closed** and all mutable ops fail —
the superseded process never races the live one. (Manifest *bodies* are already epoch-pathed, so a stray
write lands under the dead epoch's prefix and is swept; the fence additionally protects the shared mutable ref
shards.)

**Startup error text** (actionable):

```
Content-addressed disk '<disk>' cannot start: server_root_id '<id>' is actively mounted by another server.
  Existing mount: server_uuid=<…> hostname=<…> pid=<…> last_seq=<…> expires_at_ms=<…>
This prevents two ClickHouse servers from writing the same CAS namespace.
 - If the other server is still running, configure a unique <server_root_id> for this disk.
 - If it is dead, wait until the mount lease expires and retry (same server only).
 - If the local ClickHouse uuid file was regenerated, restore the old uuid file, or remove the stale owner
   object gc/server-roots/<id>/owner only after verifying no server uses this root.
```

`mount` answers "who actively owns this `server_root_id` now?"; `watermark`
(`gc/server-roots/<server_root_id>/watermark`, body `server_root_id, server_uuid, writer_epoch, min_active,
seq`) answers "which builds are active for GC?". Different cadence/semantics → separate colocated objects.

### `writer_epoch` allocation (durable monotone, in a separate sticky object) {#writer-epoch-alloc}

`writer_epoch` is a **durable, strictly-monotone counter per `server_root_id`**, **never reused**. It lives in
its **own sticky object** `gc/server-roots/<server_root_id>/epoch`, **never auto-deleted** — deliberately
**not** in the recreatable `mount` lease, so it cannot reset when the lease expires or is cleared. A writable
`Store::open` allocates its epoch by CAS-bumping that counter; each open gets a unique, ordered value.

**Missing/corrupt-object rules (close the epoch-reset hazard):**

- A **missing or corrupt `epoch` (or `owner`) object while the root is non-empty is `CORRUPTED_DATA`** — never
  silently recreate it. Recreating a deleted `epoch` would reset the counter and reissue a `writer_epoch`,
  violating `NoManifestIdReuse` against the still-present old-epoch manifests. Recreate the counter **only** if
  the root is provably empty (no `cas/refs/<id>/…` and no `cas/manifests/<id>/…`).
- A missing `mount` object is **benign** — it is a lease, not the allocator; recreate it by reclaim.

So three objects with three different lifetimes: `owner` (immutable identity, `putIfAbsent` once, never
rewritten), `epoch` (monotone counter, CAS-bumped per writable open, never deleted), `mount` (lease,
expirable/recreatable). This makes the Phase-3 identity invariant provable by construction (not by
random-collision improbability) and gives GC a real ordering for "superseding/dead incarnation."
`WriterEpochMonotoneUnique` is a **mandatory** TLA+ invariant ([below](#tla)).

## Target layout {#target-layout}

```
<pool>/
  cas/
    refs/
      <server_root_id>/store/<u3>/<table_uuid>@cas@/
        0                          # RefShard objects — the ONLY thing hot GC discovery lists
        1
    manifests/
      <server_root_id>/store/<u3>/<table_uuid>@cas@/
        <writer_epoch>/
          <build_sequence>/
            000001.proto           # PartManifest bodies — cold; cursor-swept
            000002.proto
  roots/
    <server_root_id>/
      … ordinary mirrored / verbatim files (e.g. _files/metadata_version.txt, deduplication_logs/…) …
  blobs/
    <aa>/<blob_hash>               # content-addressed bodies — pool-global, shared across servers
  gc/
    server-roots/
      <server_root_id>/
        owner                      # sticky: immutable identity (server_uuid), putIfAbsent once
        epoch                      # sticky: durable monotone writer_epoch counter, never deleted
        mount                      # lease: active heartbeat (liveness), expirable/recreatable
        watermark                  # active-build floor (writer_epoch + min_active) for the manifest sweep
    registry                       # namespace universe + registry fence (UNCHANGED authority)
    state                          # GcState (+ best-effort manifest_sweep_cursor)
    gen/<generation>/attempt/<attempt>/…
  _pool_meta
  # NOTE: legacy `trees/` (TreeId-keyed) is dead in the single-owner-manifest model — no live writer/reader;
  # ObjectKind::Tree is a vestigial switch arm. This design neither uses nor extends it (see Scope).
```

Concrete `@cas@` example, `server_root_id = server-01`, table uuid `3f2e9b1c-…@cas@`:

```
# AFTER — hot discovery lists only:
cas/refs/server-01/store/3f2/3f2e9b1c-…@cas@/0
cas/refs/server-01/store/3f2/3f2e9b1c-…@cas@/1
# cold + verbatim elsewhere:
cas/manifests/server-01/store/3f2/3f2e9b1c-…@cas@/<writer_epoch>/17/000001.proto
roots/server-01/store/3f2/3f2e9b1c-…@cas@/_files/metadata_version.txt

# BEFORE — LIST roots/ walked manifests + verbatim per namespace:
roots/<uuid-derived-ns>/0
roots/<uuid-derived-ns>/_manifests/<writer_instance_id>/17/9a/9a8f…c1.proto
```

A recursive `LIST cas/refs/` is `O(total ref shards)`; the cursor-key parse (`<ns>/<shard>`, split on the last
`/`, shard numeric) is unchanged.

## Terms {#terms}

- **server_root_id** — required, validated, immutable config; the explicit identity/layout boundary every
  namespace is rooted at.
- **RefShard** — the mutable authoritative shard for refs: current `ref_name → PartManifestRef` index + the
  ordered owner-transition log GC folds. (Today's "root shard"; relocated + re-rooted, not redefined.)
- **PartManifest** — immutable full file list of one part (paths, blob hashes, sizes, inline entries).
- **PartManifestRef** — `(writer_epoch, build_sequence, manifest_ordinal)`.
- **PartManifestId** — `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)`. Since
  `root_namespace` starts with `server_root_id`, server identity is not duplicated in `writer_epoch`.
- **writer_epoch** — a **durable monotone** per-`server_root_id` counter allocated at writable mount claim
  ([above](#writer-epoch-alloc)); never reused. No `<server_hex>:` prefix.
- **manifest_ordinal** — fixed-width decimal per build, `000001.proto … 999999.proto`, allocated monotone
  within one `Build`, never reused; cap `999999` then fail closed with `LIMIT_EXCEEDED`. Replaces the random
  `UInt128` `manifest_instance_id` + `<aa>` fan-out.

## Implementation phases {#phases}

The spec documents the full target; implementation ships in independently-reviewable phases, identity/mount
foundation first, the load-bearing manifest-identity reshape last.

### Phase 0 — server-root identity + mount safety (foundational) {#phase-0}

- Add the required, validated, immutable `content_addressed.server_root_id` config; demote `ServerUUID` to
  owner-token-only.
- Create `gc/server-roots/<server_root_id>/{owner, epoch, mount, watermark}`; relocate the per-server
  watermark there from `roots/<server-hex>/_watermark` (watermark body carries `writer_epoch` + `min_active`).
- Implement the [single-owner gate](#owner-marker) + the [mount lease](#mount-lease) with the exact rules
  above (same-UUID reclaim only after expiry; same-UUID *live* lease fails closed; foreign UUID fails closed
  regardless of expiry), the background renewer, the [renew-failure policy](#mount-lease), and the actionable
  error text.
- Allocate `writer_epoch` from the [sticky `epoch` counter](#writer-epoch-alloc) (NOT the lease); enforce the
  missing/corrupt-object rules (missing `owner`/`epoch` over a non-empty root ⇒ `CORRUPTED_DATA`).
- Implement the [write-path lease fence](#write-fence): mutable CAS/PUT proceed only while this process's
  `(server_uuid, writer_epoch)` is the live mount and the local lease is unexpired; on supersession the disk
  trips to lost/fail-closed.
- **Build-death lookup is by `root_namespace → server_root_id → gc/server-roots/<server_root_id>/watermark`**,
  NOT by parsing a writer id out of a manifest key (the sweep code must resolve the watermark from the
  namespace's `server_root_id`, since `writer_epoch` no longer carries `server_hex`).
- **TLA+ gate** for the ownership/lease discipline (see [TLA+](#tla)).
- Establishes `server_root_id` as the configured identity; the physical key relocation is Phase 1.

### Phase 1 — A: re-rooted relocation of refs + manifests (identity-preserving) {#phase-1}

- Namespace derivation prefixes `server_root_id`. `CasLayout`: `rootShardKey` → `cas/refs/<ns>/<shard>`;
  `manifestKey` → `cas/manifests/<ns>/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto`
  (**keep current ManifestRef fields + `<aa>` fan-out** — only the prefix moves and re-roots). `roots/` keeps
  `<server_root_id>/…` verbatim/mirrored files. New `casRefsPrefix()`/`casManifestsPrefix()`.
- `Gc::listRootShardTokens` LISTs `casRefsPrefix()`; token-diff comparison unchanged.
- **fsck + namespace-ops follow the relocation** (non-optional): fsck enumerates `cas/refs/` + `cas/manifests/`
  + `roots/` + `blobs/` separately; "namespace empty?"/drop consults the split; the manifest-sweep build-death
  lookup uses the Phase-0 `server_root_id → watermark` rule.
- `gc/registry` unchanged; manifest identity unchanged (objects moved, identities untouched) ⇒ no
  manifest-identity TLA+ change.
- **Recommended companion:** fix `CasObjectStorageBackend::list` to pass a real `max_keys` (server-side
  pagination) instead of `max_keys=0` + client slicing — every prefix LIST O(N²/page) → O(N).

### Phase 2 — C: cursor-paced bounded orphan-manifest sweep {#phase-2}

Replace the per-round full-namespace `pickOneSweepTarget`/`sweepNamespace` scan with a cursor-paced budgeted
pass over `cas/manifests/`, **same liveness authority as today** ([below](#sweep-liveness)).

The cursor is **best-effort cleanup state, not safety state**. It lives in `GcState` for convenience but
**`fold`, `retire`, and `recheck` must not depend on it**; losing/resetting it only causes a repeated scan,
never a reachability change.

```
GcState { … ; manifest_sweep_cursor : string }   # best-effort; NOT consulted by fold/retire/recheck
```

Two independent budgets (LIST is far cheaper than destructive ops, which can hit HEAD fallback + exact-token
retries): `manifest_sweep_list_budget_keys` (N) and `manifest_sweep_delete_budget_keys` (M). Cadence: **after**
the round's safety-critical completion + retention prune.

```
page = LIST cas/manifests/ after manifest_sweep_cursor, limit N
for key in page:
    if delete_budget exhausted: break
    if malformed key:                                continue
    if not provably build-dead (watermark by server_root_id): continue   # prefixEligible
    if referenced by active owner / pending removal: continue            # activeManifestKeys
    deleteExact(key, listed_token if present else HEAD token)            # NotFound/TokenMismatch spared
manifest_sweep_cursor = page.next_cursor
if page.end: manifest_sweep_cursor = ""                                  # wrap
```

Failure modes are benign: round aborts before the `gc/state` CAS → cursor progress lost (re-scan — fine); two
leaders race → only the accepted `gc/state` cursor wins (fine); any uncertainty → fail open / skip.

### Phase 3 — B: `writer_epoch` + `manifest_ordinal` identity reshape {#phase-3}

- Rename `writer_instance_id` → `writer_epoch` (now the Phase-0 durable monotone counter, no `<server_hex>:`
  prefix); update codecs, `PartManifestRef`, `RefMatchesBody`, parsers, tests.
- Replace the random `manifest_instance_id` + `<aa>` with the per-build monotone `manifest_ordinal`:
  `cas/manifests/<ns>/<writer_epoch>/<build_sequence>/<ordinal>.proto`. `Build::stageManifest` allocates
  `000001…`, no reuse, cap `999999` → `LIMIT_EXCEEDED`. Body repeats `PartManifestRef` + `root_namespace`;
  `RefMatchesBody` mandatory.
- **Identity recheck:** `NoManifestIdReuse` becomes **allocator uniqueness** of
  `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)`, now provable by construction —
  `server_root_id` (config, owner-gated) + `writer_epoch` (**durable monotone**, never reused, allocated at
  mount claim) + `build_sequence` (strictly increasing per process) + `manifest_ordinal` (fresh per-`Build`
  monotone). No reliance on random-epoch non-collision. The TLA+ `ManifestIds` stays an opaque per-namespace
  id ⇒ targeted invariant recheck, not a model rewrite.

## S3 budget {#s3-budget}

- **Hot discovery, before:** `GET gc/registry` + recursive `LIST roots/` (≈ all ref shards + the whole
  `_manifests/` backlog + verbatim files), with the `max_keys=0` client-paging multiplier.
- **Hot discovery, after:** `GET gc/registry` + `LIST cas/refs/` (≈ `namespaces × root_shards` keys only),
  independent of backlog size; with the `max_keys` fix, ~one page-RPC per ~1000 shards.
- **Cold sweep:** bounded `LIST cas/manifests/` after the cursor, ≤ N keys + ≤ M deletes per round.
- **Startup:** `putIfAbsent`/`GET owner` (one tiny object) + `GET`/CAS `mount`; renewer = one CAS per
  `mount_renew_period`.

## Sweep liveness (manifest-body protection) {#sweep-liveness}

A PartManifest body's liveness is **owner-state + build-death + delete-after-sealed-decrements ordering** —
**not** blob in-degree (blob in-degree governs *blobs*; manifest bodies are governed by owner events). The
sweep uses the same protect test as today (`CasOrphanManifestSweep`), with the watermark resolved via the
namespace's `server_root_id` (Phase 0):

- **Build-death (eligibility) — compare `writer_epoch` FIRST, then `build_sequence`.** With durable ordered
  epochs the test is two-level against `gc/server-roots/<server_root_id>/watermark` (which carries the
  current `writer_epoch` + `min_active`):
  - `manifest.writer_epoch < watermark.writer_epoch` → an **older, superseded epoch** → **dead/eligible**
    regardless of `min_active` (this is what reclaims orphan manifests left by a crashed prior process —
    without the epoch compare, an old-epoch build whose `build_sequence ≥` the current `min_active` would
    **leak forever**);
  - `manifest.writer_epoch == watermark.writer_epoch` → current epoch → eligible **iff**
    `min_active > manifest.build_sequence`;
  - `manifest.writer_epoch > watermark.writer_epoch` → a **future** epoch the watermark has not yet caught up
    to → **not eligible** (skip; treat as anomalous/`CORRUPTED_DATA`-adjacent, never delete).
  Missing watermark ⇒ **not** eligible (never a frozen-seq guess).
- **Owner-state protection (never sweep):** every committed ref's `manifest_ref` in `RootShard.refs`; every
  live precommit binding (a precommit `new_binding` not later removed below the sealed fold cursor); and every
  **pending** precommit removal whose `-1` is **not yet sealed/folded** (`transition_version >` sealed cursor)
  — so the GC fold can still read the body to emit that `-1` (closes the B8 race).
- **Deletes are exact-token, fail-open:** `deleteExact(key, token)`; `NotFound`/`TokenMismatch` spared.

Phase 2 changes only the cadence/pacing of this test, not the test itself.

## Safety {#safety}

- **Identity is owner-gated, clock-free, and single-object.** A different `ServerUUID` fails closed regardless
  of lease expiry; the wall-clock TTL never decides ownership, only liveness for *same-UUID* reclaim *after
  expiry*. The single sticky owner object means identity is a `putIfAbsent`/`GET`, never a LIST — no poisoned
  multi-marker state. Never auto-deleted ⇒ uuid regeneration fails loudly rather than switching roots.
- **No two live servers write the same `server_root_id`.** Same-UUID reclaim requires an **expired** lease, so
  a live double-start fails closed until expiry; a foreign UUID fails closed always; renew-failure on a foreign
  body stops writes (disk-lost).
- **A superseded/paused writer cannot mutate** — the [write-path fence](#write-fence) gates every mutable op on
  this process's `(server_uuid, writer_epoch)` being the live mount; a same-UUID renew that finds a different
  `writer_epoch` trips the disk to lost (not a retry), so an old process resuming after a long pause cannot
  race the reclaimer on the shared ref shards.
- **Identity uniqueness is by construction, not by chance.** `writer_epoch` is a durable monotone allocator in
  a **sticky `epoch` object that is never deleted** (a missing `epoch`/`owner` over a non-empty root is
  `CORRUPTED_DATA`, never a reset), so `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)` is
  unique without relying on random epoch non-collision and cannot be reset by clearing the lease.
- **Registry remains authority.** `LIST cas/refs/` is only a token accelerator and **never shrinks the
  universe**; missing/ambiguous listed ref shard → Read (fail-closed), unchanged.
- **PartManifestId stays namespace-qualified** (`server_root_id`-rooted); no cross-namespace sharing.
- **Exact-token deletes unchanged; blobs stay shared/content-addressed** ⇒ no revival race.
- **Sweep cursor is non-load-bearing** — no reachability decision reads it.

## TLA+ posture {#tla}

- **Phase 0 (mount safety) — small dedicated gate.** A focused model of the single-owner + lease discipline
  with an abstract clock + crash/restart + a second foreign server, proving:
  - `NoTwoServerUuidsOwnSameServerRoot` — at most one `ServerUUID` is ever the accepted owner of a
    `server_root_id` (the single owner object never changes uuid once set);
  - `ForeignUuidNeverAutoTakesOver` — a different `ServerUUID` never claims the mount, for **any** lease state
    including expired (no wall-clock path to an identity switch);
  - `SameUuidRestartCanReclaimExpiredMount` (liveness witness) — the same `ServerUUID` reclaims its own mount
    **only after expiry**, and a same-UUID *live* lease blocks a second claimant (the double-start guard);
  - **`WriterEpochMonotoneUnique` (mandatory)** — the `epoch`-object allocator never reissues a `writer_epoch`
    for a `server_root_id`, across crash/restart and across `mount` deletion (the counter is in the sticky
    `epoch` object, not the lease); a deleted `epoch` over a non-empty root is modeled as `CORRUPTED_DATA`,
    not a reset;
  - **`SupersededWriterMakesNoMutation` (mandatory, the write fence)** — once a `writer_epoch` is no longer the
    one in `mount` (a newer same-UUID incarnation reclaimed, or the lease lapsed), the superseded writer
    issues **no** mutable CAS/PUT. Model an old paused writer resuming concurrently with the reclaimer and show
    it cannot mutate a ref shard.
  Safety must not depend on the clock (only the same-UUID reclaim uses expiry).
- **Phases 1 & 2:** identity-preserving + cursor-non-load-bearing ⇒ **no manifest-identity TLA+ change**; the
  existing `CaGcRootLocalPartManifestCore` model treats ref/manifest objects as opaque ids and the universe as
  registry-authoritative, and never modeled the physical LIST prefix. A note records the discovery-LIST source
  moved.
- **Phase 3:** targeted recheck — `NoManifestIdReuse` → uniqueness of
  `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)` by construction, given the durable
  monotone `writer_epoch`; confirm the opaque-id assumption still holds under a per-build ordinal beneath a
  per-`server_root_id`-unique `(writer_epoch, build_sequence)`.

## Testing {#testing}

- **Phase 0:** gtest — single-owner gate (absent→claim; ours→ok via GET; foreign→fail; sticky across
  restart); mount lease (absent→claim; **same-uuid expired→reclaim; same-uuid live→fail-until-expiry**;
  foreign→fail regardless of expiry); **renew mismatch with same-uuid-but-different-`writer_epoch` → disk lost
  (writes stop), NOT retry**; **write fence — a superseded writer (its `writer_epoch` no longer in `mount`)
  issues no mutable op** (simulate a paused old process resuming after a reclaim, assert its ref-shard CAS is
  refused); `writer_epoch` strictly increases across reclaims and is **never reissued even after the `mount`
  object is deleted** (and a deleted `epoch`/`owner` over a non-empty root → `CORRUPTED_DATA`);
  `server_root_id` validation rejects bad paths; error text contains the remediation. TLA+ gate green
  (incl. `WriterEpochMonotoneUnique` + `SupersededWriterMakesNoMutation`).
- **Phase 1:** gtest — discovery LISTs `cas/refs/` and ignores `cas/manifests/` + `roots/` noise; a planted
  manifest backlog does not appear in discovery; fsck over the split classifies correctly; the sweep resolves
  the watermark via `server_root_id`; publish→fold→retire→reclaim still drains under `server_root_id`-rooted
  namespaces. Soak: hot-discovery LIST no longer scales with backlog size.
- **Phase 2:** gtest — cursor advances/wraps; list/delete budgets respected; build-dead orphan swept while
  committed/live-precommit/pending-removal manifests spared; cursor loss is a no-op for reachability.
- **Phase 3:** gtest — ordinal monotone per build, cap `999999` → `LIMIT_EXCEEDED`; `RefMatchesBody` holds;
  `(writer_epoch, build_sequence, ordinal)` round-trips; a restart's fresh `writer_epoch` cannot collide with a
  prior one. TLA+ identity recheck green.

## Scope and non-goals {#scope}

- **In scope:** required+validated `server_root_id` identity + `ServerUUID` demotion; single-owner + heartbeat
  mount protocol with the durable-monotone `writer_epoch` in a sticky `epoch` object + the write-path lease
  fence (Phase 0); re-rooted relocation A (Phase 1); cursor sweep C with epoch-then-sequence build-death
  (Phase 2); `manifest_ordinal`/`writer_epoch` reshape B (Phase 3). Recommended companion: the `max_keys`
  pagination fix.
- **`trees/` is legacy/dead** (replaced by single-owner ManifestIds; no live writer/reader; `ObjectKind::Tree`
  is a vestigial switch arm). This design neither uses nor extends it; removing it is a separate cleanup, not
  part of this change. The "no content-addressed trees/manifests" principle means we introduce **no new**
  content-addressing of trees or manifests.
- **Non-goals:** no `ServerUUID`-derived layout / no compatibility branch; no registry redesign; no new
  discovery index object; no cross-namespace manifest sharing; `blobs/` unaffected; **no automatic
  cross-`ServerUUID` mount takeover** (operator action only). CA is pre-release with no persisted data ⇒ no
  migration.
