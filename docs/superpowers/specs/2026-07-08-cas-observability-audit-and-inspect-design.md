---
description: 'Design for CAS observability improvements: complete the manifest/precommit lifecycle audit in system.content_addressed_log (emit ManifestPut + PrecommitRemoved, delete dead enum entries, fix two blob-retire audit findings) and add a clickhouse-disks ca-inspect command that decodes any CA bucket object to human-readable JSON.'
sidebar_label: 'CAS observability (audit + ca-inspect)'
sidebar_position: 53
slug: /superpowers/specs/cas-observability-audit-and-inspect
title: 'CAS observability — audit-log completion + ca-inspect design'
doc_type: 'guide'
---

# CAS observability — audit-log completion + `ca-inspect` design {#cas-observability}

Two debuggability improvements, bundled into one cycle (both serve CAS observability), motivated by having
to hand-decode bucket objects to root-cause this session's dangling-precommit and promote-over-committed
bugs. Backlogged as `INTROSPECTION-1` and `INTROSPECTION-2` in `utils/ca-soak/scenarios/BACKLOG.md`. They
are independent subsystems (event emission in the storage core vs a CLI tool) but small and related.

## Part A — INTROSPECTION-1: complete the manifest/precommit audit log {#part-a}

### Problem {#part-a-problem}

`system.content_addressed_log` (B170, see the `project_b170_cas_event_log` design) is meant to reconstruct
every entity's whole lifetime, but the manifest/precommit lifecycle is only half-instrumented (verified):
`ManifestPut` has **0 emit sites** (only the manifest DELETE is logged, via `ManifestDelete` at R6 in
`CasGc.cpp`); `PrecommitRemoved` has **0 emit sites**; `ManifestExpand`/`ManifestRetire`/`ManifestStrip` are
**dead enum entries** (declared in `CasEvent.h`, never emitted — obsolete remnants of the Merkle layer
excised in the rev.15 `PartManifest` redesign). Consequence: a manifest's birth (body write) and a
precommit's writer-side removal are invisible per-object, which is exactly why the dangling precommit
("precommit created, never removed") could not be diagnosed from SQL and required raw-object decoding.

### Fix {#part-a-fix}

1. **Emit `ManifestPut`** in `Build::stageManifest`, after the body write, via the standard
   `EventEmitter{*store}.emit([&](CasEvent & e){ … })` idiom: `type = ManifestPut`,
   `object_kind = Manifest`, `namespace_ = owning_ns.string()`, `object_hash = manifestRefDebugString(id.ref)`
   (the `writer_epoch:build_sequence:ordinal` debug string used by `ManifestDelete`), `token` = the written
   body's token, `reason = "stageManifest: part-manifest body written"`. Makes a manifest's birth visible so
   a body that is later orphaned can be traced to its `stageManifest`.
2. **Emit `PrecommitRemoved`** in `Build::abandon`, inside/after the precommit-removal `mutateShard` (the
   `old = Precommit(final_ref, build_id, manifest_ref), new = none` event): `type = PrecommitRemoved`,
   `object_kind = Root`, `namespace_ = precommit_target_ns.string()`, `ref_name = precommit_final_ref`,
   `object_hash = manifestRefDebugString(precommit_manifest)`, `reason = "abandon: precommit binding
   removed"`. This is the WRITER-side removal; the GC-side removal already logs `PrecommitReclaim`
   (`CasGc.cpp`), so together every precommit removal is now logged and "created, never removed" is a
   visible per-object gap. (Not emitted at `Store::dropRef`, which removes *committed* refs, not precommits;
   `promote`'s precommit→committed move is already covered by `BuildPublish`.)
3. **Delete the dead enum entries** `ManifestExpand`, `ManifestRetire`, `ManifestStrip` from `CasEvent.h`'s
   `CasEventType` and from the type→string map in `CasEvent.cpp`. Update the stale comment in `CasGc.cpp`
   (~L550) that name-drops `ManifestExpand` (it describes the fold's `RootAdd`/`RootRemove` blob-edge
   events, not a manifest event). No behavior change — these types were never emitted.
4. **Blob-retire audit findings (from the resurrect-fix final review), folded in:**
   - (a) In `CasBlobInDegree.cpp` `closeBlob`, the resurrect supersede currently reuses the `head_blob`
     lambda to peek the current token — but `head_blob` is the *fresh-condemn* observation hook, so it
     emits `blob_retire` (+ increments `CasGcRetiredCondemned`/`report.condemned`) IN ADDITION to the
     caller's `blob_retire_replaced` (+ `CasGcRetireReplaced`). Give the supersede a **side-effect-free
     HEAD** (a plain `backend.head`, or an observe-only mode of the peek) so `blob_retire_replaced` is the
     SOLE retire event for a supersede and the counters increment once. No functional change to the
     re-condemn itself — only the audit/counter accounting.
   - (b) `blob_retire_replaced` records only the new token; the resurrect-fix spec intended
     `{hash, old_token, new_token, round}`. Add the superseded `old_token` to the event's `detail`
     (e.g. `detail["superseded_token"]`), so the repair line is self-contained.

### Safety {#part-a-safety}

Emitting events is side-effect-free w.r.t. the CAS protocol (the sink is best-effort logging; `EventEmitter`
never participates in a CAS or blocks a decision). Deleting never-emitted enum entries is a pure cleanup.
Finding (a) *reduces* spurious side effects (one retire event + one counter increment per supersede instead
of two) and does not change which token is condemned or any delete decision. No invariant is affected.

## Part B — INTROSPECTION-2: `clickhouse-disks ca-inspect` {#part-b}

### Problem {#part-b-problem}

There is no supported way to inspect a decoded CA bucket object; diagnosing this session's bugs required an
ephemeral `mc` container + `od` hexdump + hand-parsing protobuf/custom-binary. The decoders already exist
(`decodeRootShard`, `decodePartManifest`, `decodeMountLease`, `decodeGcState`, `decodeFoldSeal`,
`decodeRetiredSet`); they are simply not exposed.

### Fix {#part-b-fix}

Add `programs/disks/CommandCaInspect.cpp` (modeled on `CommandCaGcDryRun.cpp`) and register it in
`programs/disks/DisksApp.cpp` (next to `ca-gc-dryrun`/`ca-gc-rebuild`). Usage:
`clickhouse-disks ... ca-inspect <key>` (disk selected the same way the other `ca-*` commands do). It is
**read-only** (require a read-only-opened CA disk, like `ca-gc-dryrun`/`fsck`), reads the object bytes at
`<key>`, dispatches to the correct decoder by the key's layout, and prints human-readable JSON to stdout.

Dispatch by key layout (using the pool's `Layout` prefixes, not fragile substring guesses):
- `<prefix>/cas/refs/<ns>/<shard>` → `decodeRootShard` → JSON `{shard_version, fence_round, incarnation,
  refs:[{ref_name, manifest_ref, mutable_files, published_at_ms}], journal:[{transition_version,
  old_binding, new_binding, is_tombstone}]}`.
- `<prefix>/cas/manifests/…/NNNNNN.proto` → `decodePartManifest` → `{ref, root_namespace_id, entries:[{path,
  placement, blob_hash, blob_size}], payload_digest}`.
- `<prefix>/gc/server-roots/<srid>/mount` → `decodeMountLease` → `{writer_epoch, min_active,
  observed_gc_round, expires_at_ms, gc_fenced, …}`.
- `<prefix>/gc/state` → `decodeGcState`.
- `<prefix>/gc/gen/<g>/attempt/<a>/fold_seal` → `decodeFoldSeal`; retired-set keys → `decodeRetiredSet`.
- `<prefix>/blobs/…` → the `CasEnvelope` header only (magic/size/token), NOT the payload.
- An unrecognized key → a clear error listing the recognized layouts (fail-closed; no silent guess).

`u128`/token/hash fields render as lowercase hex; the JSON uses the same field names the decoded structs use.

### Safety {#part-b-safety}

Read-only (no CAS, no delete, no mutation), consistent with `fsck`/`ca-gc-dryrun`/`ca-gc-rebuild`. A decode
failure (bad magic / wrong key type) surfaces as an error, never a partial/guessed dump.

## Testing {#testing}

- **Part A (gtest, `src/Disks/tests/`):** drive the real `Build` — `stageManifest` → assert a `ManifestPut`
  event (right `object_hash`/`token`); `precommitAdd` → `abandon` → assert a `PrecommitRemoved` event; a
  resurrect supersede (reuse the `gtest_cas_gc_leak.cpp` `ResurrectReplaced*` setup) → assert exactly ONE
  `blob_retire_replaced` (with `old_token` in `detail`) and NO accompanying `blob_retire`, and that
  `CasGcRetireReplaced` incremented once and `CasGcRetiredCondemned` did not double-count. Capture events
  via the existing test event-sink hook (the `CasEventSink` the log tests already use).
- **Part B:** a unit test that encodes a synthetic object with each encoder (`encodeRootShard`,
  `encodePartManifest`, `encodeMountLease`, `encodeGcState`) and asserts `ca-inspect`'s
  decode-and-render (the dispatch-by-key + to-JSON function, factored so it is unit-testable without the
  full CLI) produces JSON containing the expected fields; plus a manual `ca-inspect` smoke against a real
  ca-soak pool object.

## Out of scope {#out-of-scope}

- Extending `fsck` detail to report the WHY per key (`reachable-via` / `spared-by-precommit` / `eligible`) —
  a larger, separate improvement.
- Implementing the deleted `ManifestExpand/Retire/Strip` semantics (they are obsolete, not merely
  unimplemented).
- New columns/schema changes to `system.content_addressed_log` — the new events use the existing `CasEvent`
  fields (`detail` map covers `old_token`).

## Docs to update after it lands {#docs-to-update}

`utils/ca-soak/scenarios/BACKLOG.md` (`INTROSPECTION-1`/`INTROSPECTION-2` → resolved); the
`project_b170_cas_event_log` context (note the manifest/precommit lifecycle is now fully audited); a short
note wherever the `clickhouse-disks` `ca-*` commands are documented (add `ca-inspect`).
