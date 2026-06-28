---
description: Design for an explicit server_root_id-rooted content-addressed pool layout with a hot/cold split (cas/refs vs cas/manifests), a mount ownership+heartbeat startup protocol (sticky owner marker = identity, lease = liveness), a cursor-paced bounded orphan-manifest sweep, and the writer_epoch/manifest_ordinal identity reshape. GC discovery becomes a single LIST over ref shards instead of a recursive walk of roots/.
sidebar_label: CA layout hot/cold split
sidebar_position: 1
slug: /development/cas-layout-hot-cold-split-design
title: CA pool layout — server_root_id identity, hot/cold split, mount safety
doc_type: reference
---

# CA pool layout — `server_root_id` identity, hot/cold split, mount safety {#ca-layout-hot-cold-split}

## Problem {#problem}

Two problems, addressed together because they share the pool layout:

**1. GC discovery is quadratic over the mirrored tree.** The background scheduler runs a round roughly
every 2 seconds; each round first reads the per-ref-shard tokens to decide which shards changed since the
last fold (the Phase-2 token-diff: skip a shard whose listed token still equals the sealed `folded_token`).
Today ref shards live at `roots/<namespace>/<shard>`, **interleaved under the same `roots/` subtree** as the
per-build part-manifest backlog (`roots/<ns>/_manifests/…`, tens of thousands of objects) and verbatim
mirrored files (`roots/<ns>/_files/…`). So discovery does a **recursive LIST of all of `roots/`** to read a
handful of ref-shard tokens, paging the whole backlog every round — and `CasObjectStorageBackend::list`
passes `max_keys=0` (unbounded) + client-side paging, so each page re-fetches the whole prefix → O(N²/page).
This is `GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS`.

**2. Layout identity is implicitly derived from `ServerUUID`,** with a "stable unless the uuid file is lost"
footgun: if the local ClickHouse `uuid` file is regenerated, the namespace root silently switches, and
nothing prevents two live servers from mounting the same pool path and writing the same namespace tree.

## Goal {#goal}

- Make hot-path discovery cost **`GET gc/registry` + `LIST cas/refs/`**, returning *only* ref-shard objects
  (and tokens) — never manifest bodies, verbatim files, or the mirrored tree.
- Make layout identity **explicit and configured** via a required `server_root_id`; demote `ServerUUID` to a
  runtime owner token. Guard the mount with two composable startup checks: a clock-free **sticky owner
  marker** (identity) and an **active heartbeat lease** (liveness).
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

Every namespace is rooted at `server_root_id`: the namespace derivation prefixes it, so
`root_namespace = <server_root_id>/store/<u3>/<table_uuid>@cas@`. Consequences:

- **Per-server metadata is `server_root_id`-scoped** — `cas/refs/<server_root_id>/…`,
  `cas/manifests/<server_root_id>/…`, `roots/<server_root_id>/…`.
- **`blobs/` is NOT rooted** — content-addressed blob bodies (`blobs/<aa>/<hash>`) stay pool-global and
  shared across servers (the "shared blobs, per-server trees" invariant; dedup spans replicas, trees do not).
- Because the namespace already carries server identity, **`writer_epoch` drops the server prefix** and
  becomes `process_epoch` only (see [Terms](#terms)).
- No `ServerUUID` fallback, no compatibility branch, no implicit-uuid layout. CA is pre-release with no
  persisted data, so there is **no migration**.

## Mount ownership and heartbeat {#mount-safety}

Two composable startup checks protect a `server_root_id`. **The owner marker is identity; the lease is
liveness.** This is a hard rule: the lease (and its wall-clock TTL) must never decide *whether this is the
right owner* — only *whether an owner is currently live*.

### Sticky owner marker (identity, clock-free) {#owner-marker}

```
gc/server-roots/<server_root_id>/owners/<server_uuid_hex>
```

On writable startup: `putIfAbsent` our marker, then `LIST gc/server-roots/<server_root_id>/owners/`:

- **no owners** → we created ours → continue;
- **exactly ours** → continue;
- **any other owner** → **fail closed**;
- **multiple owners** (incl. malformed) → **fail closed**.

The marker is **sticky**: never auto-deleted on shutdown. If `ServerUUID` is regenerated (uuid file lost),
startup **fails** instead of silently switching the namespace root. Read-only open does not mutate but may
validate if the marker exists.

### Active mount heartbeat lease (liveness, same-UUID-only reclaim) {#mount-lease}

```
gc/server-roots/<server_root_id>/mount
```

Body: `server_root_id, server_uuid, process_epoch, hostname, pid, started_at_ms, seq, expires_at_ms`.

Startup protocol (runs **after** the owner-marker gate passes):

- **absent** → `putIfAbsent` our body;
- **same `server_uuid`** → CAS-overwrite to claim (advance `seq`, set `expires_at_ms = now + ttl`),
  regardless of the old `process_epoch` — whether expired/stale or we are simply the same server restarting;
- **different `server_uuid`** → **fail closed, regardless of expiry** (the owner-marker gate already rejected
  this; restated here so the lease branch is unambiguous).

Cross-`ServerUUID` takeover is **never automatic** — even an expired lease + foreign UUID fails closed. Reason:
`server_root_id` is an identity/layout boundary, not a transient lease; a wall-clock TTL must not decide
"same owner or new"; auto cross-UUID takeover is unsafe under clock skew, VM pause, network partition, or an
old server still alive. Failover to a different physical server is an **operator action**: restore the old
`<clickhouse_path>/uuid`, or explicitly remove the stale owner marker after verifying the old server is dead.

Background renewer (a periodic scheduled task — no `sleep`-loop): CAS-overwrite the same object every
`mount_renew_period`, advancing `seq` and `expires_at_ms = now + ttl`. **Renew-failure policy:**

- token mismatch but body still ours → bounded retry;
- body **foreign** → treat the disk as **lost**: stop accepting writes / fail closed;
- backend error (cannot renew) → fail closed for writes until recovered, per the existing disk-failure policy.

**Startup error text** must be actionable, e.g.:

```
Content-addressed disk '<disk>' cannot start: server_root_id '<id>' is actively mounted by another server.
  Existing mount: server_uuid=<…> hostname=<…> pid=<…> last_seq=<…> expires_at_ms=<…>
This prevents two ClickHouse servers from writing the same CAS namespace.
 - If the other server is still running, configure a unique <server_root_id> for this disk.
 - If it is dead, wait until the mount lease expires and retry (same server only).
 - If the local ClickHouse uuid file was regenerated, restore the old uuid file, or remove the stale owner
   marker gc/server-roots/<id>/owners/<old_uuid> only after verifying no server uses this root.
```

`mount` answers "who actively owns this `server_root_id` now?"; `watermark`
(`gc/server-roots/<server_root_id>/watermark`, body `server_root_id, server_uuid, process_epoch, min_active,
seq`) answers "which builds are active for GC?". Different cadence/semantics → kept as separate colocated
objects.

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
        <process_epoch>/
          <build_sequence>/
            000001.proto           # PartManifest bodies — cold; cursor-swept
            000002.proto
  roots/
    <server_root_id>/
      … ordinary mirrored / verbatim files (e.g. _files/metadata_version.txt, deduplication_logs/…) …
  blobs/
    <aa>/<blob_hash>               # content-addressed bodies — pool-global, shared across servers
  trees/                           # unchanged (TreeId-keyed; orthogonal)
  gc/
    server-roots/
      <server_root_id>/
        owners/<server_uuid_hex>   # sticky owner marker (identity)
        mount                      # active heartbeat lease (liveness)
        watermark                  # active-build floor for the manifest sweep
    registry                       # namespace universe + registry fence (UNCHANGED authority)
    state                          # GcState (+ best-effort manifest_sweep_cursor)
    gen/<generation>/attempt/<attempt>/…
  _pool_meta
```

Concrete `@cas@` example, `server_root_id = server-01`, table uuid `3f2e9b1c-…@cas@`:

```
# AFTER — hot discovery lists only:
cas/refs/server-01/store/3f2/3f2e9b1c-…@cas@/0
cas/refs/server-01/store/3f2/3f2e9b1c-…@cas@/1
# cold + verbatim elsewhere:
cas/manifests/server-01/store/3f2/3f2e9b1c-…@cas@/<epoch>/17/000001.proto
roots/server-01/store/3f2/3f2e9b1c-…@cas@/_files/metadata_version.txt

# BEFORE — LIST roots/ walked manifests + verbatim per namespace:
roots/<uuid-derived-ns>/0
roots/<uuid-derived-ns>/_manifests/<writer_instance_id>/17/9a/9a8f…c1.proto
roots/<uuid-derived-ns>/_files/metadata_version.txt
```

A recursive `LIST cas/refs/` is now `O(total ref shards)`; the cursor-key parse (`<ns>/<shard>`, split on the
last `/`, shard numeric) is unchanged.

## Terms {#terms}

- **server_root_id** — required config; the explicit identity/layout boundary every namespace is rooted at.
- **RefShard** — the mutable authoritative shard for refs: current `ref_name → PartManifestRef` index + the
  ordered owner-transition log GC folds. (Today's "root shard"; relocated + re-rooted, not redefined.)
- **PartManifest** — immutable full file list of one part (paths, blob hashes, sizes, inline entries).
- **PartManifestRef** — `(writer_epoch, build_sequence, manifest_ordinal)`.
- **PartManifestId** — `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)`. Since
  `root_namespace` starts with `server_root_id`, server identity is not duplicated in `writer_epoch`.
- **writer_epoch** — `process_epoch` only (the existing random-nonzero-per-`Store::open` value,
  `CasStore.cpp:99`; GC treats a different epoch as a dead process, equality only). No `<server_hex>:` prefix.
- **manifest_ordinal** — fixed-width decimal per build, `000001.proto … 999999.proto`, allocated monotone
  within one `Build`, never reused; cap `999999` then fail closed with `LIMIT_EXCEEDED`. Replaces the random
  `UInt128` `manifest_instance_id` + `<aa>` fan-out.

## Implementation phases {#phases}

The spec documents the full target; implementation ships in independently-reviewable phases, identity/mount
foundation first, the load-bearing manifest-identity reshape last.

### Phase 0 — server-root identity + mount safety (foundational) {#phase-0}

- Add the required `content_addressed.server_root_id` config; demote `ServerUUID` to owner-token-only.
- Create the `gc/server-roots/<server_root_id>/{owners/<uuid>, mount, watermark}` objects; relocate the
  per-server watermark there from `roots/<server-hex>/_watermark`.
- Implement the two startup gates ([owner marker](#owner-marker), [mount lease](#mount-lease)) with the exact
  rules above, the background renewer, the renew-failure policy, and the actionable error text.
- **TLA+ gate** for the ownership/lease discipline (see [TLA+](#tla)).
- This phase establishes `server_root_id` as the configured identity; the physical key relocation is Phase 1.

### Phase 1 — A: re-rooted relocation of refs + manifests (identity-preserving) {#phase-1}

- Namespace derivation prefixes `server_root_id`. `CasLayout`: `rootShardKey` → `cas/refs/<ns>/<shard>`;
  `manifestKey` → `cas/manifests/<ns>/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto`
  (**keep current ManifestRef fields + `<aa>` fan-out** — only the prefix moves and re-roots). `roots/` keeps
  `<server_root_id>/…` verbatim/mirrored files. New `casRefsPrefix()`/`casManifestsPrefix()`.
- `Gc::listRootShardTokens` LISTs `casRefsPrefix()`; token-diff comparison unchanged.
- **fsck and namespace-ops follow the relocation** (non-optional): fsck enumerates `cas/refs/` +
  `cas/manifests/` + `roots/` + `blobs/` separately; "namespace empty?"/drop consults the split.
- `gc/registry` unchanged; TLA+ manifest identity unchanged (objects moved, identities untouched).
- **Recommended companion:** fix `CasObjectStorageBackend::list` to pass a real `max_keys` (server-side
  pagination) instead of `max_keys=0` + client slicing — turns every prefix LIST from O(N²/page) into O(N).

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
    if not provably build-dead (watermark):          continue   # prefixEligible
    if referenced by active owner / pending removal: continue   # activeManifestKeys
    deleteExact(key, listed_token if present else HEAD token)    # NotFound/TokenMismatch spared
manifest_sweep_cursor = page.next_cursor
if page.end: manifest_sweep_cursor = ""                          # wrap
```

Failure modes are benign: round aborts before the `gc/state` CAS → cursor progress lost (re-scan — fine);
two leaders race → only the accepted `gc/state` cursor wins (fine); any uncertainty → fail open / skip.

### Phase 3 — B: `writer_epoch` + `manifest_ordinal` identity reshape {#phase-3}

- Rename `writer_instance_id` → `writer_epoch` (now `process_epoch` only — drop the `<server_hex>:` prefix,
  since the namespace carries server identity); update codecs, `PartManifestRef`, `RefMatchesBody`, parsers,
  tests.
- Replace the random `manifest_instance_id` + `<aa>` with the per-build monotone `manifest_ordinal`:
  `cas/manifests/<ns>/<process_epoch>/<build_sequence>/<ordinal>.proto`. `Build::stageManifest` allocates
  `000001…`, no reuse, cap `999999` → `LIMIT_EXCEEDED`. Body repeats `PartManifestRef` + `root_namespace`;
  `RefMatchesBody` mandatory.
- **Identity recheck:** `NoManifestIdReuse` becomes **allocator uniqueness** of
  `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)`, sound on: `server_root_id` (config,
  owner-gated) + `process_epoch` (random per `Store::open`, dead-on-mismatch) + `build_sequence` (strictly
  increasing per process, `Store::allocateBuildSeq`) + `manifest_ordinal` (fresh per-`Build` monotone). The
  TLA+ `ManifestIds` stays an opaque per-namespace id ⇒ targeted invariant recheck, not a model rewrite.

## S3 budget {#s3-budget}

- **Hot discovery, before:** `GET gc/registry` + recursive `LIST roots/` (≈ all ref shards + the whole
  `_manifests/` backlog + verbatim files), with the `max_keys=0` client-paging multiplier.
- **Hot discovery, after:** `GET gc/registry` + `LIST cas/refs/` (≈ `namespaces × root_shards` keys only),
  independent of backlog size; with the `max_keys` fix, ~one page-RPC per ~1000 shards.
- **Cold sweep:** bounded `LIST cas/manifests/` after the cursor, ≤ N keys + ≤ M deletes per round.
- **Startup:** `LIST owners/` (tiny) + `GET/CAS mount`; renewer = one CAS per `mount_renew_period`.

## Sweep liveness (manifest-body protection) {#sweep-liveness}

A PartManifest body's liveness is **owner-state + build-death + delete-after-sealed-decrements ordering** —
**not** blob in-degree (blob in-degree governs *blobs*; manifest bodies are governed by owner events). The
sweep uses the same protect test as today (`CasOrphanManifestSweep`):

- **Build-death (eligibility):** the build's `(writer_epoch, build_sequence)` is below the live floor in the
  per-server watermark (`min_active > build_sequence`, or the farewell/retired sentinel). Missing watermark ⇒
  **not** eligible (never a frozen-seq guess).
- **Owner-state protection (never sweep):** every committed ref's `manifest_ref` in `RootShard.refs`; every
  live precommit binding (a precommit `new_binding` not later removed below the sealed fold cursor); and every
  **pending** precommit removal whose `-1` is **not yet sealed/folded** (`transition_version >` sealed cursor)
  — so the GC fold can still read the body to emit that `-1` (closes the B8 race).
- **Deletes are exact-token, fail-open:** `deleteExact(key, token)`; `NotFound`/`TokenMismatch` spared.

Phase 2 changes only the cadence/pacing of this test, not the test itself.

## Safety {#safety}

- **Identity is owner-gated and clock-free.** A different `ServerUUID` fails closed regardless of lease
  expiry; the wall-clock TTL never decides ownership, only liveness for *same-UUID* reclaim. Sticky marker is
  never auto-deleted ⇒ uuid regeneration fails loudly rather than switching roots.
- **No two live servers write the same `server_root_id`.** The heartbeat lease fails a second live mount with
  an actionable error; renew-failure on a foreign body stops writes (disk-lost).
- **Registry remains authority.** `gc/registry` orders first-publish-into-a-namespace against the fence;
  `LIST cas/refs/` is only a token accelerator and **never shrinks the universe**.
- **Missing/ambiguous listed ref shard → Read** (fail-closed), unchanged.
- **PartManifestId stays namespace-qualified** (now `server_root_id`-rooted); no cross-namespace sharing;
  identity unchanged in Phases 0–2, re-grounded (allocator uniqueness) in Phase 3.
- **Exact-token deletes unchanged; blobs stay shared/content-addressed** ⇒ no revival race introduced.
- **Sweep cursor is non-load-bearing** — no reachability decision reads it.

## TLA+ posture {#tla}

- **Phase 0 (mount safety) — small dedicated gate.** A focused model of the owner-marker + lease discipline,
  proving:
  - `NoTwoServerUuidsOwnSameServerRoot` — at most one `ServerUUID` is ever an accepted owner of a
    `server_root_id` (the owner-marker set never admits a second uuid past the gate);
  - `ForeignUuidNeverAutoTakesOver` — a different `ServerUUID` never claims the mount, for **any** lease
    state including expired (no wall-clock path to an identity switch);
  - `SameUuidRestartCanReclaimExpiredMount` (liveness witness) — the same `ServerUUID` can reclaim its own
    stale/expired mount and make progress.
  Model the lease with an abstract clock + crash/restart + a second foreign server; safety must not depend on
  the clock (only `SameUuidRestartCanReclaimExpiredMount` uses expiry, and only for the same uuid).
- **Phases 1 & 2:** identity-preserving + cursor-non-load-bearing ⇒ **no manifest-identity TLA+ change**; the
  existing `CaGcRootLocalPartManifestCore` model treats ref/manifest objects as opaque ids and the universe as
  registry-authoritative, and never modeled the physical LIST prefix. A note records the discovery-LIST source
  moved.
- **Phase 3:** targeted recheck — `NoManifestIdReuse` → uniqueness of
  `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)` by construction; confirm the opaque-id
  assumption still holds under a per-build ordinal beneath a per-process-unique `(writer_epoch, build_sequence)`.

## Testing {#testing}

- **Phase 0:** gtest — owner-gate (no owner→claim; ours→ok; foreign→fail; multiple/malformed→fail; sticky
  across restart); mount lease (absent→claim; same-uuid expired→reclaim; same-uuid live→fail-until-expiry;
  foreign→fail regardless of expiry); renew-failure (foreign body → writes stop); error text contains the
  remediation. TLA+ gate green.
- **Phase 1:** gtest — discovery LISTs `cas/refs/` and ignores `cas/manifests/` + `roots/` noise; a planted
  manifest backlog does not appear in discovery; fsck over the split classifies correctly; publish→fold→
  retire→reclaim still drains under the `server_root_id`-rooted namespaces. Soak: hot-discovery LIST no longer
  scales with backlog size.
- **Phase 2:** gtest — cursor advances/wraps; list/delete budgets respected; build-dead orphan swept while
  committed/live-precommit/pending-removal manifests spared; cursor loss is a no-op for reachability.
- **Phase 3:** gtest — ordinal monotone per build, cap `999999` → `LIMIT_EXCEEDED`; `RefMatchesBody` holds;
  `(writer_epoch, build_sequence, ordinal)` round-trips; simulated-restart (fresh epoch) does not collide.
  TLA+ identity recheck green.

## Scope and non-goals {#scope}

- **In scope:** required `server_root_id` identity + ServerUUID demotion (Phase 0a); mount ownership+heartbeat
  protocol (Phase 0b); re-rooted relocation A (Phase 1); cursor sweep C (Phase 2); `writer_epoch`/
  `manifest_ordinal` reshape B (Phase 3). Recommended companion: the `max_keys` pagination fix.
- **Non-goals:** no `ServerUUID`-derived layout / no compatibility branch; no registry redesign; no new
  discovery index object; no cross-namespace manifest sharing; no content-addressed tree/manifests; `blobs/`
  and `trees/` unaffected; **no automatic cross-`ServerUUID` mount takeover** (operator action only). CA is
  pre-release with no persisted data ⇒ no migration.
