---
description: 'Design spec: fold the GC retired list into the per-shard in-degree run (3-cursor to 2-cursor settlement merge) — one artifact family, deterministic byte-equal adoption preserved via the attempt-pinning invariant, retired_refs removed from gc/state.'
sidebar_label: 'CAS retired-in-snapshot'
sidebar_position: 10
slug: /superpowers/specs/cas-retired-in-snapshot
title: 'CAS GC — Retired List Inside the Snapshot Run'
doc_type: 'reference'
---

# CAS GC — Retired List Inside the Snapshot Run {#title}

**Status:** approved design (brainstorm 2026-07-10, approach B), spec for implementation planning.
**Branch:** `cas-gc-rebuild`. **Predecessors:** v3 freshness-meta (writers no longer read the retired
list; `docs/superpowers/plans/2026-07-10-cas-freshness-meta-v3.md`), ack-floor one-pass round
(`docs/superpowers/specs/2026-07-02-cas-gc-ack-floor-design.md`), origin note
`docs/superpowers/cas/refactoring-ideas.md` §"Post-v3 GC settlement follow-ups" item 1.

## 1. Problem and goal {#problem}

After v3, the retired list is **GC-private**: the writer's condemned decision is a per-hash `.meta`
point-read (`Core/CasBlobMeta.h`), and no writer ever reads `RetiredSet`. Yet the retired list is
still a **separate durable artifact family** (`RetiredSet`, proto `RetiredSetProto`, magic `CART`,
one object per gc-shard at `Layout::retiredKey`, referenced from `GcState::retired_refs`), with its
own read/write per fold round, its own adoption rule, its own retention pruning, its own rebuild
minting, and a third cursor in the settlement merge.

Costs today (all verified in code):

- **Fold round:** 1 GET (prior retired, `CasGc.cpp:723`) + 1 PUT (next retired, `CasGc.cpp:524`)
  per gc-shard per folding round.
- **Every round including deferred:** `Gc::graduationDue` (`CasGc.cpp:1643`) GETs and decodes
  **every** retired list just to answer one boolean.
- **State:** `GcState::retired_refs` (gc-shard → key map) rides every `gc/state` CAS body.
- **Merge:** a third cursor (`prior_retired` in `foldDeltasIntoGeneration`,
  `Core/CasBlobInDegree.h:114`).
- **Duplication:** rebuild (`CasGc.cpp:2006`), retention pruning, `fsck`, `ca-inspect`,
  `previewDeletes`, and `hasInFlightRetired` each carry a retired-specific code path beside the
  run-specific one.

**Goal:** one artifact family. The per-shard in-degree run carries the condemned state; the
`RetiredSet` object, the `CART` magic, `Layout::retiredKey`, and `GcState::retired_refs` are
removed. The settlement merge becomes 2-cursor. Settlement **semantics stay byte-for-byte**:
condemn → carry → graduate (`delete_pending`) → redelete, round-paced graduation
(`condemn_round < current_round`), two-phase graduation, clamp suppression, pre-CAS single
delete site, resurrect-supersede, `.meta` writes, B170 events, and GC-log counters are unchanged.

**Non-goals** (explicitly out of scope; each was weighed in the brainstorm):

- No change to round pacing or the one-round graduation gap (it is the racing-writer
  edge-surfacing window under EDGE-BEFORE-OBSERVE — load-bearing).
- No post-CAS delete site (would need a crash catch-up path — two delete sites instead of one).
- Outcome logs (`CAGO`) stay separate: transient per-round audit, not carried state.
- No incremental/LSM snapshot (refactoring-ideas item 2 — independent, after this).
- No compatibility scaffolding: pre-release, no persisted pools to migrate
  (standing rule `feedback_ca_no_compat_scaffolding_predev`). Old pools are recreated.

## 2. Data model {#data-model}

### 2.1 Run rows {#run-rows}

The run file (`Core/CasRunFile.h`, streaming writer/reader, 32-byte keys
`srcEdgeRunKey(blob_hash, source_id)`, variable-length values) today has two row values:

- `0x01` `kEdgeActive` — a surviving active edge, key `(blob_hash, source_id)`;
- `0x00` `kZeroMarker` — blob transitioned to zero this generation, key
  `(blob_hash, kZeroSourceId=0)`; per-generation, dropped on carry.

Add one row value at the **same sentinel key** `(blob_hash, source_id = 0)`:

- `0x02` `kCondemned` — the blob's condemned incarnation, **carried across generations until
  settled**. Value encoding after the type byte:
  `[u8 flags (bit0 = delete_pending)] [u8 token_type] [u64be condemn_round] [u64be size]
  [u16be token_len] [token value bytes]`.
  `token_type` persists `Token::type` (`Core/CasToken.h`: `ETag = 1`, `Generation = 2`,
  `Emulated = 3`) — `deleteExact` consumes a full `Token{value, type}`, and the current durable
  `RetiredEntryProto` stores both `token_value` and `token_type`; dropping the type would be lossy
  (review finding, 2026-07-11). Decode fails closed (`CORRUPTED_DATA`) on an unknown `token_type`
  or a payload length that does not match the declared `token_len`.

Rules:

- **One sentinel row per blob per generation.** A condemned row *subsumes* the zero-marker
  (condemnation only happens at in-degree 0). A plain `kZeroMarker` is written only for the
  absent-at-condemn case (nothing to delete) and stays transient exactly as today.
- The sentinel key sorts **before** all real edges of its blob (`source_id 0` < any real id), so
  the streaming cursor sees the carried condemned state when it opens a blob and settles at
  blob close-out — the adjacency the 2-cursor merge needs.
- `RetiredEntry.kind` is dropped (`ObjectKind` has only `Blob`; manifests go through the separate
  owner-removal sweep, never the retired pipeline). Ordering is by `hash` (the run's native key
  order).
- Run header `key_schema` bumps `0 → 1` **for `RunKind::BlobDelta`** (`key_schema` is a per-kind
  namespace — "fixed per kind; meaning owned by the producer"; the value `1` is already used by a
  different `RunKind`, which is fine). **New requirement (review finding, 2026-07-11): the current
  readers do not validate `key_schema` at all** — `RunFileReader` only exposes `keySchema()`, and
  `zeroInDegree` / `inDegreeInGeneration` / the prior-edge cursor never check it. This spec
  mandates a typed open path for source-edge runs (validate `kind == BlobDelta`,
  `key_schema == 1`, and per-row payload lengths; anything else → `CORRUPTED_DATA` /
  `NOT_IMPLEMENTED` fail-closed) used by **every** consumer: the merge cursor, `zeroInDegree`,
  `inDegreeInGeneration`, dryrun, `fsck`, `ca-inspect`. No compat shim for `key_schema = 0` runs.

### 2.2 Fold-seal summary {#seal-summary}

`CasFoldSeal` (`Core/CasGenerationSeal.h:64`) gains a per-gc-shard summary map:

```
std::map<uint64_t /*gc_shard*/, CondemnedSummary> condemned_summary;
struct CondemnedSummary
{
    uint64_t condemned_total = 0;              /// carried kCondemned rows (incl. pending)
    uint64_t pending_total = 0;                /// subset with delete_pending
    uint64_t oldest_nonpending_condemn_round = UINT64_MAX;   /// UINT64_MAX = none
};
```

Derived **from the durable run bytes** while writing (or adopting) each shard's run, so it is a
pure function of the sealed content — the seal stays a deterministic artifact (§4). Consumers:

- `graduationDue` = `pending_total > 0 || oldest_nonpending_condemn_round < current_round`
  over the summaries — **zero I/O** (the seal is already read for `changedShardCount`).
  **Fail-closed pre-defer rule preserved (review finding, 2026-07-11):** today a missing retired
  list makes `graduationDue` return `true` ("force a fold so the round's fail-closed path surfaces
  it, never silently defer", `CasGc.cpp:1649-1652`). The seal-summary version keeps the same
  posture: for `snap_generation > 0`, a missing or undecodable adopted seal — or a seal without a
  `condemned_summary` — makes `graduationDue` return `true` (the forced fold then fails closed on
  the real integrity loss). An idle pool must never defer forever over lost settlement state.
- Pure ref-carry condition = "no deltas AND `condemned_total == 0`" — the exact analogue of
  today's `!folded_any && prior_retired[shard].empty()` (`CasGc.cpp:1104`).
- `hasInFlightRetired`, `ca-inspect` counts — O(1) from the seal.

### 2.3 `GcState` shrink {#gcstate-shrink}

`GcState::retired_refs` is removed; the proto field number is `reserved` (never reused), matching
the existing convention (`cas_format.proto`: `reserved 10` for `observed_gc_round`). `GcState`
becomes `{round, fence_seq, gc_shards, snap_generation, snap_pruned_through, snap_attempt,
manifest_sweep_cursor, lease}`. `RetiredSetProto`, `encodeRetiredSet` / `decodeRetiredSet`, the
`CART` magic, and `Layout::retiredKey` are deleted. `05-formats-and-backend.md` records `CART`
as removed (same style as `CATR`).

## 3. Merge and round flow {#merge}

`foldDeltasIntoGeneration` (`Core/CasBlobInDegree.h:114`) loses the `prior_retired` input — the
prior run **is** the retired input now. Two cursors: prior run (edges + condemned rows) ×
scattered deltas. The cursor stashes the sentinel row when it opens blob `h`, merges edges, and
settles at close-out with **today's rules in today's order**:

1. `delete_pending` (from the prior durable run) → **redelete** at d = 0: the caller executes the
   exact-token delete pre-CAS and the row is not carried. d > 0 for a pending row is structurally
   impossible — spared + loud log (as today).
2. d > 0 → **spared**: row not carried (recovery wins even past the pacing gate).
3. d = 0 and `condemn_round < current_round` → **graduated**: republished as `delete_pending`
   (two-phase graduation; deleted the next pass).
4. d = 0 otherwise → carried unchanged.

Fresh zero-transition: `head_blob` observation → `kCondemned` row minted at `condemn_round`
(absent object → plain `kZeroMarker`, nothing to condemn — as today). `peek_head` /
resurrect-supersede (`ReplacedEntry`) unchanged. **Clamp suppression unchanged**: with
`suppress_destructive`, pending rows carry unchanged and floor-passed rows stay condemned-only;
condemnation and sparing continue.

`RetiredMergeResult` (the `graduated` / `spared` / `redelete` / `replaced` outputs) is kept as the
merge's report interface — the round consumes it exactly as today for: the outcome log, `.meta`
writes (`writeCondemnedMeta` at condemn, spare/delete transitions on the bounded
`gc_meta_pool_size` pool), B170 events, and the GC-log counters (`entries_condemned`,
`entries_graduated`, `entries_redeleted`). **No observable behavior changes**; the `05008`
stateless invariant (`sum(entries_redeleted) >= sum(objects_deleted)`) must pass unmodified.

**Load-bearing discipline (encoded in the TLA+ gate):** destructive actions (redelete) execute
**only** against `delete_pending` rows read from the *prior durable adopted* run; fresh
observations (`head_blob` tokens) are write-only in the pass that mints them and become actionable
only after durable adoption. The acting round always re-reads the token from the durable artifact.
This is today's retired-list semantics carried over.

## 4. Adoption and integrity {#adoption}

**The merged run stays a deterministic artifact — the adoption rule does not change at all.**
(Amended after external review, 2026-07-10: the first draft proposed first-write-wins adoption
with the integrity anchor moved to the seal; that rested on a false premise and is withdrawn.)

- **Run:** `putDeterministicArtifact` unchanged — `putIfAbsent`; on `PreconditionFailed`
  byte-equal-or-`CORRUPTED_DATA` (`Core/CasBlobInDegree.h:29`).
- **Seal:** `putDeterministicArtifact` unchanged; `RunRef.checksum` and the `condemned_summary`
  are pure functions of the written run bytes.
- **`gc/state` CAS:** unchanged — the single CAS adopts `{round, snap_generation, snap_attempt}`;
  the condemned state adopts *through the seal* (named by generation + attempt). The one-pass
  round property is preserved; there is no `retired_refs` left to adopt.

**Why byte-equality cannot false-fire on observation content — the attempt-pinning invariant
(load-bearing, encoded in the TLA+ gate):** the fold mints its artifact keys from
`attempt = lease.seq`, and the renew/steal paths bump `lease.seq` **every round**
(`CasGc.cpp:809-814`, `:1372`) — a failed or repeated round always lands under fresh keys. A
`putIfAbsent` collision at the same key can therefore only be a same-execution resend of the
**same buffer** (a network-level retry) — byte-identical by construction. There is no reachable
"same-attempt replay that re-observes a different `head_blob` token": a re-execution is a new
attempt with new keys. Consequently:

- byte-equality never trips on legitimate operation, and it **hard-catches** the incoherence class
  an external review raised against the first draft (stale edge bytes silently adopted under a
  newer sealed coverage — under byte-equal adoption this is `CORRUPTED_DATA`, fail-closed, the
  round aborts and the next round re-folds under a fresh attempt);
- the replay-divergence hazard for side effects does not exist **within an attempt**: the merge
  result whose bytes are durable is the same in-memory result that drives the `.meta` writes, B170
  events, and outcome tallies — run/meta/event divergence inside one attempt is structurally
  impossible. (Independently, `BlobMeta` carries no token — `{state, condemn_round, size}` — so
  condemned-meta content is observation-independent anyway.)
  **Scope (review finding, 2026-07-11):** this claim is per-attempt, not per-adopted-round. Meta
  writes complete BEFORE the round's `gc/state` CAS by design (`CasGc.cpp:508-512` — the writer's
  point-read gate must see condemns no later than the ledger), so a **deposed** leader leaves
  advisory `.meta`/event effects from an unadopted attempt — exactly as it does today; this
  refactor does not change that. Those effects are safe by the existing v3 meta-race argument: a
  stray `Condemned` only causes an unnecessary resurrect (INV-1-conservative), destructive deletes
  never key off the meta (exact-token from the durably adopted run only), and meta transitions are
  etag-CAS-guarded. The TLA+ gate models meta effects as advisory (§6).

The historical "observation-bearing ⇒ first-write-wins" classification of the retired set
(`Core/CasBlobInDegree.h:27-28`) becomes obsolete for the relocated state: with attempt-pinned
keys the condemned rows inherit the deterministic-artifact rule. (The outcome log keeps its
existing defensive adopt path — out of scope.)

**Crash-completeness:** unchanged — a crash between run PUT and seal/state CAS leaves orphan
attempt-scoped artifacts that retention prunes (`snap_pruned_through`, B174), exactly as today.

## 5. Consumers {#consumers}

| Consumer | Today | After |
|---|---|---|
| Fold round | `retired_refs` → GET per shard (`CasGc.cpp:723`) | prior runs (already read) |
| `graduationDue` (`CasGc.cpp:1643`) | GET + decode every retired list, every round | seal summaries, zero I/O |
| Pure ref-carry (`CasGc.cpp:1104`) | "no deltas AND retired empty" | "no deltas AND `condemned_total == 0`" |
| `previewDeletes` (dryrun, `CasGc.cpp:1646`) | retired GETs | stream seal-named runs, filter `kCondemned` |
| `fsck` (`CasFsck.cpp:277`) | `retired_refs` loop | same runs it already streams for reachability — one pass |
| `ca-inspect` (`CasInspect.cpp:239`) | `retired_refs` map dump | per-shard summaries from the seal (+ optional row streaming) |
| `hasInFlightRetired` (`CasGc.h:308`, tests) | GET + decode | seal summary, O(1) |
| Rebuild runbook (`CasGc.cpp:2006`) | mints `RetiredSet` objects + `retired_refs` | writes `kCondemned` rows into the fresh runs it already writes (+ seal it already writes) |
| Retention prune (B174) | prunes runs **and** retired objects | one family; retired prune code deleted |

Observability is unchanged: B170 events (`blob_retire`, `blob_retire_replaced`, `blob_delete`),
GC-log counters, `.meta` writes, `system.content_addressed_garbage_collection_log` schema.

## 6. TLA+ gate (phase 0, before any code) {#tla-gate}

New small model `docs/superpowers/models/CaRetiredInRun.tla` (+ cfg, runner script per the
`run_*.sh` convention, `tmp/tla2tools.jar`), extending the ack-floor/round family's abstractions:

- State: per-shard adopted artifact = `{edges, condemned rows}` as one atom; `gc/state` names the
  adopted generation; deterministic (byte-equal) adoption; attempt ids minted fresh per round
  (the attempt-pinning invariant, §4); seal derived from the written bytes.
- Actions: fold (2-cursor settle: condemn / spare / graduate / redelete), round failure + retry
  (new attempt, new keys), same-buffer resend (byte-identical collision), competing leader
  (attempt-scoped keys — no cross-leader key collision), clamp suppression
  (`suppress_destructive`), pure ref-carry (no rewrite when nothing to settle), writer
  resurrect (fresh incarnation between condemn and delete → exact-token delete misses,
  outcome `Replaced`).
- Invariants: `INV_NO_LOSS` (no referenced blob deleted), `INV_NO_RETURN` (no stale token ever
  deletes a live incarnation), one-pass adoption (settled state visible iff the round's CAS
  committed), coverage-coherence (the sealed coverage always describes the adopted run bytes),
  and the write-only-fresh-observations discipline (a delete's token always equals the durably
  adopted condemn-time token).
- Meta effects are modeled as **advisory** (a deposed leader's pre-CAS `.meta` writes from an
  unadopted attempt exist in the state space): no destructive transition may read the meta; a
  stray `Condemned` may only trigger a resurrect (safe direction). Liveness side: `graduationDue`
  returns due on missing/undecodable adopted settlement state (never an eternal defer).
- Sabotage flips (must go red): (a) redelete uses an in-memory token instead of the durable one;
  (b) attempt-pinning broken — a re-execution reuses the prior attempt's keys at an advanced
  journal cut → must surface as a byte-equal `CORRUPTED_DATA` refusal (never a silent adoption of
  stale edges under newer coverage); (c) graduate without the `condemn_round < current_round`
  gate → racing-writer edge loses its spare window.

Gate is green (invariants hold; every sabotage flips red) **before** implementation starts.

## 7. Testing {#testing}

- **gtests** (`CasBlobInDegree` suites): condemned rows in the merged stream (carry, settle order,
  zero-marker subsumption, absent-at-condemn), **`Token{value, type}` round-trip through the
  `kCondemned` row for all three `TokenType`s + fail-closed on an unknown `token_type` and on a
  truncated payload**, deterministic-adopt collision (byte-identical resend adopts; divergent
  bytes throw), seal summary derivation, **the typed open path rejecting wrong
  `kind`/`key_schema` from every consumer** (merge cursor, `zeroInDegree`,
  `inDegreeInGeneration`, dryrun, `fsck`, `ca-inspect`), `graduationDue` fail-closed on a
  missing/corrupt adopted seal, round tests (graduate/redelete over the in-run state, clamp
  suppression carries pending), rebuild path.
- **e2e:** `05008_ca_gc_snap_prune` must pass **unmodified** (its invariant is
  settlement-semantics-only). Point-run the CA-s3 stateless CAS tests (`04286`, `05008`, `05009`).
- **Soak:** phase-1 (`utils/ca-soak`, 2-replica, `run_phase1.sh`) with all checkpoints
  `fsck dangling=0 / unreachable=0`; S30/S33-class scenario re-run (concurrent-leader,
  resurrect-churn).
- **Docs:** `04-gc-protocol.md` (§retired, §heartbeat sections), `05-formats-and-backend.md`
  (`CART` removed, run row table, seal summary), `07-s3-budget.md` (op table: retired GET/PUT rows
  removed, `graduationDue` cost), `ROADMAP.md` follow-ups entry.

## 8. Implementation phases (for the plan) {#phases}

1. **Phase 0:** TLA+ gate (§6) — model green + sabotage red.
2. **Phase 1:** run format — `kCondemned` row, `key_schema` 1, writer/reader/cursor, unit tests.
3. **Phase 2:** merge and round — 2-cursor `foldDeltasIntoGeneration`,
   seal `condemned_summary`, `graduationDue` / ref-carry from summaries, round wiring.
4. **Phase 3:** consumers — `fsck`, `ca-inspect`, `previewDeletes`, `hasInFlightRetired`,
   rebuild; delete `RetiredSet` / `CART` / `retiredKey` / `retired_refs` (proto `reserved`).
5. **Phase 4:** validation — full `*Cas*` gtest battery, CA-s3 point-runs, phase-1 soak, docs.
