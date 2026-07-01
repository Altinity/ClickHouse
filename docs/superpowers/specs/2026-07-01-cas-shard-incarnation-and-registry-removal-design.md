# CA GC shard incarnation + registry removal (D1: dropNamespace never deregisters) — design

**Status:** DESIGN (2026-07-01). Phase-0 TLA+ gate **GREEN** (see below); implementation phases 1–5 not yet started.
**Branch:** `cas-layout-hot-cold-split`.
**Phase-0 gate result (2026-07-01):** `docs/superpowers/models/CaGcShardIncarnationCore.tla` + `CaGcShardIncarnationCore_RESULTS.md`. The `_design` config holds `INV_NO_DANGLING` and `INV_NO_ORPHAN_EDGE` across 724,944 distinct states — **THM-NO-RETURN holds without the registry**, so the registry is deleted (the §risks `pending-newborns` fallback is not needed). Three negative controls each break the invariant they target: `SabotageNewbornNoFloor` (round self-floor irreducible), `SabotagePathKeyedCursor` (incarnation irreducible — ABA), `SabotageDeleteBeforeFold` (fold-before-reclaim ordering). The **one-vs-two question is answered: two** — neither coordinate alone suffices.
**Origin:** D1 in the CA GC debt inventory — `dropNamespace` tombstones a namespace's shards but never removes it from `gc/registry`, so the registry grows monotonically (soak scenario S30) and GC's per-round fence/fold fanout is proportional to *every table ever created*, not the live ones. The empty tombstoned ref-shard objects and the registry entries accumulate as the unreachable "other" residual.
**Supersedes/reframes:** the original D1 framing ("add a safe deregister step to the registry"). Investigation showed the registry's monotone-authority role is itself the defect; this design removes it rather than patching a deregister onto it.

## Problem {#problem}

Three facts, all confirmed in code:

1. **`dropNamespace` never deregisters** (`CasStore.cpp:942-995`). It tombstones each touched shard (appends removal `RootOwnerEvent`s + clears refs via `mutateShard`), deletes verbatim files, evicts the decode cache — but leaves the namespace in `gc/registry`. The comment at `CasStore.cpp:1064` explicitly defers cleanup to a "full GC (M-F)" that was never implemented.

2. **GC discovers FROM the registry, and fences the cartesian product.** `discoverUniverse` (`CasGc.cpp:1277-1291`) enumerates `registry.namespaces × shardsToVisit`. The fence step (`CasGc.cpp:632-648`) CAS-bumps `fence_round` into *every shard of every registered namespace* — minting a fence-only manifest for absent shards. So per-round GC cost ∝ (namespaces ever created) × `root_shards`.

3. **GC never reclaims the ref-shard objects.** GC `deleteExact`s blobs (`CasGc.cpp:800`), part-manifests (`:927`), and its own retired-set/generation debris (`:1011`, `:1188`) — but nothing deletes `cas/refs/<ns>/<shard>` objects. A dropped namespace's emptied shard objects persist forever.

Net: the registry entry + the empty ref-shard objects are per-dropped-namespace, and grow without bound under a create/drop workload (S30 = the "other" residual, e.g. the S05 240-object plateau). This is a scalability defect, not a correctness bug: no data loss, `fsck dangling=0` holds.

## Why the obvious fixes were rejected {#rejected}

- **Writer deregisters at `dropNamespace`.** Unsafe: the removal events are the only carrier of the `-1` blob-in-degree edges; GC folds them only in the recheck window `(folded_cursor, fence_version]`. If the namespace vanishes from discovery before GC folds that window past the fence, the `-1`s are lost → blobs keep phantom in-degree → permanent blob leak. Deletion must be GC-driven, only after the journal is folded past a completed fence.

- **GC hard-deregisters when "empty + settled."** An empty-but-live namespace (a freshly created table with no inserts yet) is indistinguishable from a dropped one by emptiness alone. Inferring "empty ⇒ retire" would deregister a live table between `CREATE` and the first `INSERT`.

- **Both of the above plus M-C3 (GC trims empty shard objects) on the *current* path-keyed cursor.** Deleting and later recreating an object at the stable path `cas/refs/<ns>/<shard>` is an ABA hazard: the fold cursor is keyed by path and filters journal events by `transition_version` (`CasGc.cpp:718-722`), and a recreated shard resets `shard_version` to 0. Old sealed `folded_cursor = K` would silently skip the new incarnation's events (versions `1..≤K`) → lost `±1` edges → dangle or leak. Plus the recreated shard's `fence_round` resets to 0, reopening the create-ordering ("no-return") race.

The root observation: the ABA hazard, the create-ordering fence, and the monotone registry are all consequences of a resettable per-shard coordinate plus a separate monotone namespace-set. Fix the coordinate and the rest collapses.

## Core design: two coordinates, not five {#core}

Today "fence" conflates five overlapping counters (`shard_version`/`transition_version`, per-shard `fence_round`, `registry_version`, registry `fence_round`, GC `round`) and two special mechanisms (registry-as-discovery-authority, fence-mints-absent-shards). Collapse the whole thing to **two orthogonal coordinates**:

- **`incarnation`** — a durable, monotone, never-reused coordinate stamped into a ref-shard object at (re)creation and immutable for that object's life. It carries **identity**, closes **ABA** by construction, and orders **shard birth**. Source: the `(writer_epoch, build_sequence)` of the build that first creates the shard object — both are already durable-monotone-never-reused per `server_root_id` (`writer_epoch` via `allocateWriterEpoch`, `CasServerRoot.cpp:245-259`; `build_sequence` monotone within an incarnation). This adds **no new persistent object**. (Fallback if the `(writer_epoch, build_sequence)`-strictly-increases-on-recreate invariant cannot be guaranteed: a dedicated sticky per-`server_root` incarnation counter using the exact `allocateWriterEpoch` pattern. Preferred is reuse; the fallback is the escape hatch.)

- **GC `round`** — the pool-global clock (unchanged). It is the writer's retire-view gate floor: no writer may durably reference a blob condemned as of round `R` without first refreshing its retire-view to `≥ R`. This is pool-global and cross-writer, so it CANNOT be a per-`server_root` epoch — it stays the GC round.

The fold cursor is keyed by **`(ns, shard, incarnation)`** (today: `(ns, shard)`). A recreated shard draws a strictly-greater incarnation, so an old sealed cursor can never match it → the fold always processes a new incarnation from zero. ABA is closed *by construction* — no "forget the cursor on delete" bookkeeping is needed.

## §1 — Incarnation {#incarnation}

**Field.** `RootShard` gains `incarnation` (a `(uint64 writer_epoch, uint64 build_sequence)` tuple, or an opaque monotone `UInt128` if the fallback allocator is used). New protobuf field in `RootShardManifest`; codec encodes/decodes it; fail-closed on absent-where-required (CA is pre-release — no compat scaffolding).

**Stamping.** Set once, at the CAS create-if-absent that first materializes the shard object. Never mutated by later builds writing into the same object. On delete + recreate, the new create stamps the then-current build's coordinate, which is strictly greater.

**Cursor keying.** `CasFoldSeal::per_ns_shard` and `completion_seal.folded_cursors` key by a cursor key that includes the incarnation. `cursorKey`/`parseCursorKey` extended accordingly. Fold reads the shard, reads its incarnation, and locates the prior cursor under `(ns, shard, incarnation)`; a mismatch (new incarnation) means "fold from 0".

**Invariant (INC-MONO):** for a fixed `(ns, shard)`, every successive materialization of the object carries a strictly greater incarnation. This is the ABA-closure obligation — proven in TLA+ and asserted in a gtest that drops + recreates a shard.

## §2 — Discovery from refs; delete the registry {#discovery}

- `discoverUniverse` becomes `LIST(cas/refs/)` — the set of `(ns, shard)` that physically exist, each self-identifying by its incarnation. `listRootShardTokens` (`CasGc.cpp:1303`) already does this LIST as an accelerator; it becomes the authority. Ambiguity detection (a key seen twice across pages forces Read) is retained (fail-closed).
- The fence step (§5) fences only discovered shards, not `registry × root_shards`.
- **`gc/registry` is deleted entirely**: `RootsRegistry` struct + `CasRootsRegistry.{h,cpp}` codec, `rootsRegistryKey`, `ensureRegistered`/`registered_cache`, the registry-fence sub-step (`CasGc.cpp:602-630`), and the `_registry` cursor special-case (`CasGc.cpp:712`). The namespace universe is now exactly "the shards that exist."
- **Consumer migration:** `listNamespaces` (`CasStore.cpp:1060`) currently reads the registry (used for FREEZE shadow-tree enumeration and opaque namespace listing). It migrates to a LIST over `cas/refs/` (+ `roots/` for verbatim-only namespaces), exactly as `listMirroredChildren` (`CasStore.cpp:1078`) already unions the two subtrees. This must be verified against every `listNamespaces` caller.

**Backend requirement (LIST-CONSISTENCY):** discovery correctness now rests on strongly-consistent LIST (read-your-writes for enumeration). S3 provides this. RustFS and `InMemoryBackend` must be confirmed/made to provide it; if a backend cannot, it is unsupported for this pool layout (fail-closed, documented — not a silent fallback).

## §3 — Newborn namespace = a precommit-state shard (no separate object) {#newborn}

The registry's one irreducible role was **create-ordering of a shared blob**: a first publish into a brand-new namespace dedup-references an existing blob `B`; concurrently another namespace drops its last ref to `B`; if GC's fold does not see the newborn's `+1` before condemning `B`, the newborn's ref dangles. A LIST alone does not close this (no atomic point between the shard PUT and GC's LIST).

We reuse the **existing precommit machinery** instead of a registry:

- Precommit is already first-class: `OwnerKind::Precommit` in the journal, a `+1` held under the **fold barrier** (control #23) until the body is present (`CasGc.cpp:363-371`), promoted to `Committed` on the same `manifest_ref` (owner move) once the body appears, and reclaimed if abandoned via the `(writer_epoch, build_sequence)` **watermark** (`reclaimAbandonedPrecommit`, `CasGc.cpp:1604-1658`).
- A first publish **creates the ref-shard object at its canonical path carrying a precommit binding** (with the fresh incarnation). The PUT makes the shard LIST-discoverable — it is an ordinary shard from discovery's point of view. There is **no separate `pending-newborns` object**.
- **Abandoned newborn** (writer crashed before committing) = an abandoned precommit → reclaimed by the existing watermark path → the shard empties → tombstone-reclaimed (§4). No bespoke "drain pending" logic.
- **Create-ordering** is the existing precommit gate: the writer does not promote (does not durably commit the reference to `B`) until it has refreshed its retire-view to the current GC round. The fold barrier symmetrically withholds the precommit `+1` until the body is present.

**Consequence:** the registry disappears completely (§2), rather than shrinking to an ephemeral set. This is the maximal simplification and is contingent only on the TLA+ theorem below.

## §4 — Reclaim the ref-shard object like a blob {#reclaim}

- `dropNamespace` appends, as the **last** journal event, an explicit **tombstone** marker (a `RootOwnerEvent` variant meaning "namespace dropped; no owner"), after the per-ref removal events. It does NOT delete the object.
- GC, during fold, when a shard is **empty (no refs)**, its **last journal event is the tombstone**, and its journal has been **folded past a completed fence**, issues `deleteExact(rootShardKey(ns, shard), token)` — the same exact-token discipline GC already uses for blobs/manifests. The `-1` removal edges were folded before/at the same pass, so no journal is lost.
- **Revive/append races the delete:** any writer append (revive) changes the token → `deleteExact` returns `TokenMismatch` → the delete is refused, the object survives with the new content, the fold continues normally. The tombstone-as-last-entry invariant guarantees a revive appends *after* the tombstone, so a revived shard is never mistaken for a dropped one.
- **Recreate after delete:** a later first publish recreates the object with a strictly greater incarnation (§1). The `(ns, shard, incarnation)` cursor never matches the deleted incarnation's cursor → fold from 0. ABA closed.
- **Idle-but-live** (all parts dropped, table alive): the shard is empty but has **no tombstone** → not reclaimed → still listed → still discovered. No "drop vs idle" inference is needed; the tombstone *is* the drop signal.

## §5 — Fence step rework {#fence}

- Remove the registry-fence sub-step (`CasGc.cpp:602-630`) and the `registry × root_shards` fence loop that mints fence-only manifests for absent shards (`CasGc.cpp:632-648`).
- Fence only **discovered** shards (the `LIST(cas/refs/)` universe). A shard that does not exist is not fenced and not minted — its birth is ordered by the precommit gate (§3), not by a fence-time mint.
- `fence_version[round][ck]` continues to record the per-shard `shard_version` observed at fence time (the fold-window upper bound); `ck` now carries the incarnation. The per-shard `fence_round` field's role as the *writer's retire gate floor* is retained (it is coordinate 2); its role in *create-ordering* is gone (subsumed by the precommit gate).

## §6 — Invariants and TLA+ obligations {#invariants}

Extend `CaGcRootLocalPartManifestCore.tla` (or a focused sibling) with actions `CreateShardIncarnation`, `Publish`/`PrecommitThenPromote`, `ReclaimShard` (tombstone + token-guarded delete), and `Recreate` (delete then re-create at the same path with a greater incarnation), plus the shared-blob create/drop race.

Obligations to prove:

- **THM-NO-RETURN (central).** The re-founded no-return theorem: with the registry removed, for a first publish that dedup-references a blob `B`, either (a) the newborn shard is in round `R`'s LIST universe and GC folds its precommit `+1` before condemning `B`, or (b) the writer observed a retire-view floor `≥ R` and does not commit the reference to `B` (re-uploads instead, per the resurrect invariant [[feedback_ca_resurrect_invariant]]). No interleaving of `PUT precommit-shard ↔ GC LIST ↔ condemn B` dangles a live ref. This replaces the registry-fence's ordering argument with the precommit-gate + incarnation + strongly-consistent LIST.
- **INV-NO-DANGLING.** No live ref ever addresses a deleted object (blob, manifest, or ref-shard). Extends the existing dangling invariant to ref-shard deletion.
- **INV-NO-ABA (INC-MONO).** Keying the fold cursor by `(ns, shard, incarnation)` with strictly-increasing incarnations never skips a new incarnation's events.
- **INV-NO-BLOB-LEAK.** Every `-1` removal edge is folded before its carrying shard object is deleted (the token-guarded delete + fold-past-fence ordering). Relies on the already-proven idempotent edge-set fold to make the crash window (delete committed, cursor bookkeeping not yet) safe.

TLA+ is a **phase-0 gate**: the model and its checked invariants land **before any implementation** (§8 phase 0), because they decide whether the registry can be removed at all. The model's first job is to test the **one-vs-two-coordinates** question: does the incarnation alone carry the ordering (one coordinate), or is the pool-global round irreducible (two)? The design claims two; the model confirms or refutes it rather than asserting it.

## §7 — What gets deleted (simplification ledger) {#deletions}

Net entities removed, not added:

- `gc/registry` object, `RootsRegistry` struct, `CasRootsRegistry.{h,cpp}`, `rootsRegistryKey`, `encodeRootsRegistry`/`decodeRootsRegistry`.
- `Store::ensureRegistered` + `registered_cache`/`registered_mutex`.
- The registry-fence sub-step and the `_registry` cursor special-case in GC.
- The fence-mints-fence-only-manifest-for-absent-shards path.
- The "M-F full-GC deregister" TODO (`CasStore.cpp:1064`) — obsolete.

Added: one immutable `incarnation` field on `RootShard`, one tombstone journal-event variant, incarnation in the cursor key. No new persistent objects (given the reuse-`(writer_epoch, build_sequence)` decision).

## §8 — Phasing {#phasing}

TLA+ comes **first**, as a gate. Implementation phases are informed by the validated model, and each is independently testable (TDD, gtests under `src/Disks/tests/`).

0. **TLA+ gate (blocks everything) — DONE, GREEN (2026-07-01).** `CaGcShardIncarnationCore.tla` models the collapsed two-coordinate design (newborn born fenced to `gcRound`, LIST-fence of present shards, incarnation-keyed fold cursor, tombstone + fully-folded token-guarded reclaim, shared-blob create/drop race). The `_design` config holds `INV_NO_DANGLING` + `INV_NO_ORPHAN_EDGE` (724,944 states) — **THM-NO-RETURN holds without the registry, so registry removal proceeds** (no `pending-newborns` fallback). Three negative controls confirm the model's teeth and that **both** coordinates are irreducible (one-vs-two → two). See `CaGcShardIncarnationCore_RESULTS.md`.
1. **Incarnation carrier.** Add `incarnation` to `RootShard` + proto + codec; stamp at create; key the fold cursor by `(ns, shard, incarnation)`. Tests: codec round-trip; INC-MONO on drop+recreate; fold picks a fresh cursor on incarnation change.
2. **Discovery from refs + registry deletion.** Switch `discoverUniverse` and the fence loop to `LIST(cas/refs/)`; delete the registry and migrate `listNamespaces`/FREEZE enumeration. Tests: discovery equals the LIST universe; a namespace with no shard object is absent from discovery; FREEZE enumeration parity.
3. **Newborn = precommit-shard.** First publish creates a precommit-state shard; create-ordering via the precommit promote-gate; abandoned newborn reclaimed by the existing watermark. Tests: a newborn's precommit `+1` protects a dedup-referenced blob from a concurrent last-ref drop; abandoned newborn drains.
4. **Tombstone reclaim.** `dropNamespace` writes the tombstone; GC token-guarded `deleteExact` of empty + tombstoned + folded-past-fence shards; revive-races-delete; recreate-after-delete. Tests: dropped namespace's shard objects are deleted; revive aborts the delete; recreate uses a greater incarnation and folds from 0; idle-but-live shard is retained.
5. **Scenario + soak validation.** S30 residual → 0 (registry + empty shard objects both gone); a create/drop churn scenario shows bounded storage and per-round GC cost ∝ live namespaces; full soak green (`fsck unreachable=0, dangling=0`).

## §9 — Testing strategy {#testing}

- **Unit (gtest):** codec round-trip incl. incarnation + tombstone; INC-MONO; cursor-fresh-on-incarnation-change; token-guarded reclaim (delete / TokenMismatch-abort / NotFound-tolerant); newborn precommit protection; abandoned-newborn reclaim; idle-but-live retention.
- **`gc_shards>1`:** every new fold/discovery test runs with `gc_shards>1` as well as `1` (closes the known `gc_shards=1` blind spot [[feedback_review_blindspots_shards_chassert]]).
- **TLA+:** §6 obligations, TLC-checked (`tmp/tla2tools.jar`, java 21).
- **Scenario suite:** a dedicated create/drop-churn scenario asserting `reclaimable == 0` *and* bounded "other" (registry residual gone); S30 converges.
- **Soak:** rebuilt binary, fresh restart per [[reference_ca_soak_fresh_restart]].

## Out of scope {#out-of-scope}

- The vestigial `/trees/` scaffolding (`ObjectKind::Tree`, `treeKey`, `MAGIC_TREE`) — no producer exists; a separate cleanup, unrelated to D1.
- The O(buffer) run-file streaming debt (A1/A3) — separate deferred backlog.
- D2 (scenario-threshold triage) and D3 (standalone `gc_shards>1` fold test) — D3 is partly absorbed here (§9 runs new tests sharded), but the broad existing-fold sharded test remains its own item.

## Open questions / risks {#risks}

- **INC-MONO via `(writer_epoch, build_sequence)`** depends on a recreate always occurring under a strictly greater build coordinate. If a drop+recreate can occur within one build without advancing `build_sequence`, use the dedicated sticky incarnation allocator (the fallback). Resolve in phase 1 by inspecting the build/publish path.
- **LIST-CONSISTENCY** is now load-bearing. Confirm RustFS and `InMemoryBackend` semantics in phase 2; document any unsupported backend as fail-closed.
- **THM-NO-RETURN** is the make-or-break: if the precommit-gate cannot be shown to close the shared-blob create race without the registry-fence, fall back to keeping an ephemeral `pending-newborns` object (the prior design's §3). The **phase-0 TLA+ gate** decides this before any implementation begins — the registry deletion is never even started on an unproven ordering argument.
