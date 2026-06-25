# CAS build-heartbeat (`builds/<build_id>`) removal — design

**Status:** design (operator-approved direction, 2026-06-26; ready for an implementation plan)
**Date:** 2026-06-26
**Branch:** `cas-vfs-path-mapping`
**Backlog:** the B203 finding (the build heartbeat is write-only / unread). Relates to B168/B201
(op-count), B167a (`farewell`), and the format-freeze work (removes one protobuf object class + magic).

## Goal

Remove the per-build heartbeat object (`builds/<build_id>`) and all its machinery. It is **written but
never read**: `decodeHeartbeat` has no live caller, nothing LISTs `builds/`, and the full-GC "M-F"
debris-reclamation consumer it was built for was never wired. Incremental GC already spares in-flight
builds via the watermark `min_active` (anchored before the first PUT) + the precommit-set
(`CasGc.cpp:1911`), independent of the heartbeat. Worse than pure op-waste: `publish()` does **not**
discard the heartbeat (only `abandon()` does — `CasBuild.cpp:92`), so a successful build leaves its
`builds/<build_id>` key behind for a reader that does not exist — an accumulation, not just per-build
PUT/renew/DELETE op cost.

Operator decision (2026-06-26): when full-GC M-F debris reclamation is eventually built, it will judge
liveness from `min_active` + the precommit-set — **not** a per-build heartbeat. So the heartbeat is
deleted outright, not preserved.

This is **pre-release**: no on-disk/wire data to stay compatible with; no migration; clean delete (the
project's no-compat-scaffolding rule).

## Non-goals

- Implementing full-GC M-F debris reclamation (separate future work; it will key off `min_active` +
  precommit).
- Touching the watermark, the seq-assign/retire machinery, `SingleWriterSlot`, or the GC-lease pulse
  `gc/hb` (`GcHeartbeat`) — all kept.
- Changing any GC delete decision (the heartbeat has no live consumer, so removal is decision-neutral).

## The one invariant to preserve

Removing the heartbeat must **not** weaken sparing of an in-flight build's not-yet-published blobs.
It does not: that sparing is performed by the watermark `min_active` (anchored synchronously **before**
the first object PUT — `CasWatermark.h`) plus the precommit-set, and incremental GC already judges
`build_seq < min_active ⇒ reclaimable, else live` (`CasGc.cpp:1911`) with no reference to the heartbeat.
The heartbeat has **no live reader**, so deleting it changes no current GC decision.

## Removal map

**Delete entirely:**
- `Core/CasHeartbeat.{h,cpp}` — `Heartbeat` struct, `encodeHeartbeat`/`decodeHeartbeat`,
  `HeartbeatKeeper`.
- `Core/CasBuild.{h,cpp}` — the `std::unique_ptr<HeartbeatKeeper>` ctor parameter + member; the
  `renewOnce()` at startBuild (`:107`); the slow-op heartbeat sanity block (`:949-956`); `abandon()`'s
  `stopBackground()` + `discard()` (`:1109-1110`); the heartbeat text in the `startBuild` / `abandon`
  `CasEvent` emissions. `abandon()` remains (it still retires the build_seq / drops staging); only its
  heartbeat calls go.
- `Core/CasStore.{h,cpp}` — `createBuild` stops constructing the `HeartbeatKeeper` (`:267`) and drops
  the heartbeat-keeper `startBackground` (`:269-270`); the `Build` ctor drops the parameter.
  **CORRECTION (grounded 2026-06-26): KEEP `heartbeat_period` and `background_heartbeats` in `PoolConfig`
  — they are SHARED with the watermark** (`CasStore.cpp:114-115` drives `watermark->startBackground`
  with them; `ContentAddressedMetadataStorage.cpp:355` sets `background_heartbeats`). Removing them
  would break the watermark's background renewal. Optional follow-up: rename to
  `watermark_renew_period` / `background_watermark` for clarity (not required; out of scope here).
- `Core/CasLayout.h` — `buildHeartbeatKey` + the `builds/` prefix documentation line (the whole
  `POOL/builds/` namespace is gone).
- Proto `Core/Proto/cas_root_shard.proto` — the `HeartbeatProto` message.
- `Core/CasFormat.{h,cpp}` — `FormatId::Heartbeat`, its `magicFor` arm (`CAHB`), its `changePoints`
  arm. Renumber the trailing `FormatId` enumerators (RootsRegistry/GcOutcomes) to stay contiguous —
  pre-release, the enum value is not persisted (only the now-removed `CAHB` magic was), so renumbering
  is safe.
- `Core/CasEvent.{h,cpp}` — the heartbeat-specific `CasEventType` arms (keep `GcLeaseHeartbeat` — that
  is the GC lease, unrelated).
- `Core/CasInstrumentedBackend.{h,cpp}` — the `/builds/` key-routing metric branch + any `CasBuild*`
  heartbeat ProfileEvents specific to `builds/`.
- The heartbeat gtests (e.g. `gtest_cas_*` cases exercising `encode/decodeHeartbeat` /
  `HeartbeatKeeper` / `builds/`).

**Keep (explicitly):**
- `ServerWatermark` + `WatermarkKeeper` (the `min_active` floor — load-bearing).
- `SingleWriterSlot` (the watermark keeper still derives from it).
- The build_id mint + seq-assign/retire machinery (`builds_mutex`, the in-memory active set feeding
  `min_active`). **`build_id` itself stays** — it is the precommit ref name / staging key, minted in
  `createBuild` independently of the keeper; only its *heartbeat object* is removed.
- `GcHeartbeat` / `gc/hb` (the per-pool GC-lease liveness pulse — a different object).

## Testing

- Full `Cas*`/`Ca*` gtest sweep stays green (baseline-only red `CaWiringOps.FreezeViaHardLinksIntoShadow`
  tolerated). Heartbeat tests are deleted, not weakened.
- Confirm the precommit-reclaim coverage that exercises `min_active`-based sparing still passes; if
  coverage is thin, add a test: a build uploads blobs, does **not** publish, abandons → GC leaves the
  blobs intact while `build_seq ≥ min_active`, and reclaims them once the seq retires (raising
  `min_active` past it). This is the test that proves the heartbeat was not load-bearing for sparing.
- A short docker-safe soak (scoped compose project; never touch the operator's `archeology` container):
  assert the `builds/` prefix is never created, op-count on `builds/` is zero, the aggregate no-loss
  oracle stays green (no in-flight over-deletion), and overall request volume drops.

## Risks / open items

- **`FormatId` renumber:** verify nothing persists or hard-codes the numeric `FormatId::Heartbeat=10`
  value beyond the `CAHB` magic (which is removed). Expected clean (the enum drives the in-memory magic
  table + change-points only).
- **`abandon()` after heartbeat removal:** confirm `abandon()` still correctly retires the build_seq /
  drops staging without the heartbeat discard — i.e. the seq-retire is not entangled with
  `heartbeat->discard()`. (Map during implementation; they are separate today.)
- **`CasEventType::Heartbeat` is the DEFAULT of `CasEvent::type`** (`CasEvent.h:45`) and is never
  emitted (startBuild emits `BuildStart`). Removing the enumerator requires repointing that default to
  another enumerator (e.g. `BlobPut`) — every emit sets `.type` explicitly, so the default is cosmetic.
- **Config keys stay** (watermark-shared — see the corrected removal map); no config-XML removal needed.
- No migration concern (pre-release).

## Verification (definition of done)

Build clean (`-Werror`); the `Cas*`/`Ca*` sweep green; `grep -rn "Heartbeat\|heartbeat\|builds/"
src/Disks/.../ContentAddressed/ | grep -v -E "GcHeartbeat|gc/hb|GcLeaseHeartbeat"` returns nothing in
non-test code; the soak shows zero `builds/` activity and a green no-loss oracle.
