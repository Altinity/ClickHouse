# CAS: GC baseline guard + raw rebuild (`gc/state` disaster recovery) — spec + plan

**Status:** DESIGN approved in brainstorm 2026-07-03 (scope (а)+(б) core, FORCE for (в)/(г);
`SYSTEM` surface; live-conservative). Supersedes the §Raw GC Rebuild part of the deleted
2026-06-30 raw-audit RFC (recovered copy: `tmp/raw_audit_gc_rebuild_rfc_recovered.md`) — that text
predates the ack-floor redesign (fence/recheck/registry are gone; heartbeat acks did not exist).
**Branch:** `cas-gc-rebuild` (off the current CAS line when implementation starts).

## Problem

Journals are TRIMMED below sealed fold cursors (INV-JOURNAL-COVERAGE): the folded history exists
ONLY as the in-degree snapshot named by `gc/state`. Losing `gc/state` on a lived-in pool is
therefore not recoverable incrementally — and worse than unrecoverable today: an absent `gc/state`
means "fresh pool", so a fresh GC would fold only the surviving journal tails, see every
long-committed blob at in-degree 0, and **mass-delete live data**. A tiny lost object escalates
into pool loss: fail-catastrophic, not fail-closed.

Failure scenarios:
- **(а)** `gc/state` absent or undecodable — the core case;
- **(б)** `gc/state` healthy but a generation artifact it references is lost/corrupt (run, fold
  seal, retired list) — GC rounds already fail closed (`CORRUPTED_DATA`), forever;
- **(в)** `gc/state` REGRESSED (restored from an old backup while mounts carry newer acks);
- **(г)** operator wants a clean GC history on a healthy pool (after a logical bookkeeping bug).

(а)+(б) share one recovery path and are the plain command; (в)/(г) are the same rebuild consciously
run over a live state — `FORCE` mode.

## Part 1 — the guard (fail-closed, independent, ships first)

A fold adopting an EMPTY baseline (`gc/state` absent ⇒ round 0, no snapshot) must REFUSE when any
discovered ref shard proves trimmed history:

```
journal.front().transition_version > 1        (events below the front were trimmed)
OR (journal empty AND shard_version > 0)      (trimmed to empty)
```

⇒ `CORRUPTED_DATA`: "ref shard journals prove trimmed history but there is no GC baseline —
`gc/state` was lost; run SYSTEM CONTENT ADDRESSED GC REBUILD". GC rounds stop; the pool keeps
serving reads and writes (the guard gates GC only, never server startup). A genuinely fresh pool
passes (all journals start at version 1).

Part of the same task: AUDIT the (б) paths — `snap_generation > 0` with an ABSENT adopted fold
seal, or absent run/retired objects, must all be `CORRUPTED_DATA` (the retired-ref path already is;
verify the seal and run paths, close any that silently treat absence as empty).

## Part 2 — `Cas::Gc::rebuildBaseline` (the recovery)

Runs under the GC lease (single leader — free mutual exclusion vs regular rounds). Writes ONLY the
gc plane (runs/seals/state); never touches ref shards, manifests, or blobs; never deletes content.
Live-conservative BY DESIGN — no quiesce mode exists:
- events landing after the rebuild's read of a shard lie above its recorded cursor → the first
  regular round folds them (the same publish-before-cut argument as a normal fold);
- a live precommit whose body is not yet present → CLAMP that shard's cursor below the precommit's
  transition (the existing fold-barrier semantics);
- fresh condemnations cannot graduate until every mount acks past the rebuilt round (the floor).

### Algorithm

1. Acquire the GC lease.
2. Discover namespaces/ref shards via LIST (the registry is gone — D1).
3. Health check of the CURRENT state: decodes AND its seal + runs + retired lists HEAD-present.
   Healthy ⇒ refuse without `FORCE`. Broken in any way ⇒ proceed plain.
4. Per ref shard: read once (record `shard_version` + token), replay owner state (committed refs +
   live precommit bindings from the journal — the `promote` owner-check replay semantics).
5. Per committed owner: read the manifest body, validate (`refMatchesBody`,
   `manifestNamespaceMatches`), emit `(blob_hash, source_id)` edges. **Missing/invalid body ⇒
   REFUSE the whole rebuild** (that is data loss; rebuild must not bless it — fsck forensics
   first). Per live precommit: body present ⇒ include edges; absent ⇒ clamp that shard's cursor
   below the precommit transition.
6. Edge streaming, memory O(budget) not O(edges): accumulate per-gc-shard edges up to a budget
   (~256 MB), sort, spill as temp run segments; final pass merges segments per gc-shard into ONE
   run via the T2 streaming readers + `RunFileWriter`.
7. Mint numbering:
   - `new_round = max(observed_gc_round over ALL mount bodies, fence_round over all shards,
     numbers of surviving gc/gen prefixes) + 1` — REQUIRED by the ack-floor: a low round would let
     stale mount acks (from the lost history) instantly float fresh condemnations past the floor
     without any writer having observed the new list;
   - `generation`/`attempt` above any surviving `gc/gen/` prefix (LIST) so deterministic-artifact
     `putIfAbsent` never collides with debris.
8. Write the synthetic fold seal: per-shard `folded_cursor = observed shard_version` (or the
   clamp), `blob_target_runs` = the merged runs.
9. `gc/state`: retired_refs **EMPTY** (over-protect: everything re-condemns from scratch through
   the normal condemn → floor → graduate → delete pipeline), cursors from the seal, the minted
   round/generation/attempt. CAS create-if-absent for (а); token-CAS over the observed broken/old
   state for (б)/(в)/(г).
10. Old-era `gc/` debris (orphan retired lists, superseded generations) is left in place — visible
    to fsck, reclaimed by the existing retention/prune paths where applicable.

### Refusal conditions (fail-closed, diagnostic report always produced)

- any committed owner names a missing or invalid `PartManifest`;
- a ref shard fails to decode;
- any mount body unreadable (cannot compute a safe round);
- healthy current state without `FORCE`;
- lease not acquired.

## Command surface

`SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE] <disk>` — follows the existing
`SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION` (Manual trigger) pattern: same access control level,
runs through `CasGcScheduler` so it emits Start/Finish rows into
`system.content_addressed_garbage_collection_log` (outcome, durations, per-run ProfileEvents) and
`gc_rebuild` audit events into `system.content_addressed_log`. The core lives in
`Cas::Gc::rebuildBaseline`.

**`clickhouse-disks ca-gc-rebuild --disk <name> [--force]`** ships in the same iteration (user
decision 2026-07-03) — the outage-independent path. Key constraint: the tool must NOT claim the
server-root MOUNT (a live server owns it; this is exactly why `fsck` demands a read-only open).
So the disks command opens the disk READ-ONLY (no mount claim, no heartbeat, no scheduler), takes
the GC LEASE (its own single-writer slot, independent of mounts), and performs gc-plane writes at
the `Cas::Gc`/backend level — the read-only gate protects the METADATA-STORAGE mutation API, not
GC-lease-guarded maintenance. A live server's GC leader and the tool exclude each other via the
lease ("another leader holds the lease" ⇒ refusal). Output: the same diagnostic report on stdout;
exit code nonzero on refusal.

## TLA+ gate (Phase 0)

Extend `CaGcAckFloorCore` with `GRebuild`: enabled at `gcPhase = idle`; effect
`retired' = {}`, `round' = MaxObservedAck + 1` (modeled as `max wAck` + 1), folded set preserved
(the model's `folded` IS the owner-derived edge truth the rebuild recomputes). Prove stage-1 clean
(all invariants) + witness (`W_RebuildHappens`) + three sabotages, each MUST yield a counterexample:
- `SabotageRebuildDropEdge` — rebuild loses one committed owner's folded ref ⇒ `INV_NO_DANGLE`;
- `SabotageRebuildKeepRetired` — rebuild carries the old retired entries ⇒ wrong-token deletes /
  `INV_NO_RETURN`;
- `SabotageRebuildLowRound` — `round' = 0` ⇒ stale acks graduate fresh condemnations unobserved ⇒
  `INV_NO_DANGLE`.

## Testing gates

1. Guard: lived-in InMemory pool (publish → rounds → trim happened) + delete `gc/state` ⇒ the next
   round throws `CORRUPTED_DATA` naming the rebuild command; nothing deleted. Fresh pool ⇒ rounds
   run as today.
2. Rebuild (а): same setup ⇒ `rebuildBaseline` ⇒ subsequent regular rounds converge: dropped blobs
   reclaimed, live blobs intact (`runRoundsUntilAbsent` + fsck `dangling == 0`), round strictly
   above all mount acks.
3. Rebuild (б): corrupt ONE run object (or drop a retired list) under a healthy state ⇒ regular
   round fails closed ⇒ plain rebuild recovers ⇒ convergence as in (2).
4. FORCE: healthy state ⇒ plain rebuild REFUSES; `FORCE` rebuilds; convergence.
5. Refusals: missing committed manifest body ⇒ rebuild refuses, report names the owner; live
   precommit without body ⇒ rebuild succeeds with a clamped cursor; the barrier releases after the
   body lands (next regular round folds it).
6. Live-writer race (unit): a publish lands between the rebuild's shard read and its `gc/state`
   CAS ⇒ the event is above the recorded cursor and the next regular round folds it (no lost +1).
7. Memory: spill/merge path exercised with a small budget (forced multi-segment merge, byte-equal
   to the in-memory result).
8. Full `Cas*` suites green; soak scenario later (kill `gc/state` mid-soak, rebuild, converge).

## Plan

- **Task 0 — TLA+ gate**: `GRebuild` + witness + 3 sabotage cfgs in `CaGcAckFloorCore`; stage-1
  clean, witness fires, sabotages refute, existing sabotages still refute.
- **Task 1 — the guard**: empty-baseline coverage check in the fold path + the (б) absence audit
  (seal/runs) + tests (gate 1). Ships independently — converts catastrophe to refusal even before
  the rebuild exists.
- **Task 2 — `Gc::rebuildBaseline`**: discovery, owner replay, edge spill/merge, numbering mint,
  refusals, state CAS + tests (gates 2-7).
- **Task 3 — SYSTEM command**: parser/interpreter wiring after the existing Manual-GC pattern,
  scheduler logging row, `gc_rebuild` audit event.
- **Task 4 — `clickhouse-disks ca-gc-rebuild`**: read-only open + GC lease + `rebuildBaseline`;
  `--force`; lease-conflict refusal test (a held lease ⇒ clean refusal, nothing written) + docs
  (`04-gc-protocol.md` §rebuild, `08-testing-and-soak.md` operator section, ROADMAP row) + memory
  update.

## Non-goals

- Repairing data loss (missing reachable blobs / manifest bodies) — that is fsck forensics, never
  automated blessing.
- Quiesced mode (live-conservative is the only mode).
- Old-era debris sweeping inside the rebuild (existing retention/fsck paths own that).
