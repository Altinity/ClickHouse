---
description: 'v7 design for the LIST-incompleteness release blocker: per-namespace contiguous ref ids, in-band epoch seals that CAS-occupy the next slot, a namespace catalog with incarnations, and a per-namespace checkpoint object with a full state machine — LIST is a zero-trust hint everywhere; two new object kinds, both off the append hot path.'
sidebar_label: 'CAS ref contiguous-chain'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: contiguous ref streams and the in-band epoch seal'
doc_type: 'reference'
---

# CAS: contiguous ref streams and the in-band epoch seal {#cas-ref-contiguous-chain}

**Date:** 2026-07-28. **Status:** v7 — v6 plus the round-6 algorithms (incarnations chosen, `_ckpt`
state machine, sequencer-legal removal reconciliation); awaiting review round 7 and user review.
**Branch:** `cas-gc-rebuild`.

Fixes BACKLOG `{#list-as-journal-dataloss-2026-07-25}` (observed:
`reports/2026-07-26-list-incompleteness-investigation.md`) and closes the rev.4 `Late Predecessor
PUT` limitation. Realizes P4 of `cas/draft-fixes-20260726.md` within its refutation condition: zero
added requests on the append path and on the fold's per-record path. Two new object kinds
(`ref_catalog`, `_ckpt`), both off the append hot path.

## 1. Problem and shape {#problem}

GC folds what a listing returned and seals a cursor above what it OBSERVED; a hidden `-1` is a
permanent leak, a hidden `+1` deletes acked data. Recovery, the orphan sweep, REBUILD and fsck consume
the same untrusted listings. Root cause (converged on independently twice): **absence is undecidable
in a sparse id space** — the pool-wide `next_ref_sequence` makes per-namespace gaps the norm, so every
hole demanded another certificate (the rejected v1–v4 stack, §13). v7 changes the invariants so
absence and committed-ness are decidable from arithmetic, point reads, and conditional writes — the
operations the store performed honestly even while its LIST lied.

## 2. Invariants {#invariants}

**INV-1 — per-namespace contiguous ids.** Next id = `greatest_applied.ref_sequence + 1` (already
computed as `commitRefChunk`'s trial preview); the pool-wide atomic is deleted; within
`(namespace, epoch)` durable ids are dense `1..T`. **Reuse only under the every-attempt rule**
(restoring the 2026-07-11 contract): an id is freed only when nothing was sent or every sent attempt
has its own conclusive rejection; a definite rejection AFTER an ambiguous attempt keeps the outcome
`Unresolved` and the lane wedged. **Wedge liveness (r6-7):** an all-attempts-ambiguous wedge retries
conditional creation of the SAME `(key, bytes)` under the original fence generation with bounded
backoff — success or an identical exact read resolves durable; foreign bytes resolve conclusive
rejection; the id is never freed by timeout. Remount remains the backstop.

**INV-2 — every epoch transition is closed in-band.** First touch after ANY writer-epoch transition
CAS-walks the dead epoch's tail via a dedicated **`slot-occupy`** primitive returning
`Created | Occupied(bytes, token) | Unresolved` (the immutable-object controller's
foreign-bytes-throw is not bent to this job). `Occupied` → adopt and replay (a landed straggler, or
an already-present `EpochSeal`, which terminates the walk); `Created` → the seal occupies `(E, T+1)`
and the `Late Predecessor PUT` ghost can never land — the store's conditional create is the fence.
**`EpochSeal` grammar (r6-10):** a seal transaction contains exactly one seal operation — no manifest
edges, no lifecycle operations; `prev_epoch_seal` is REQUIRED on exactly sequence 1 of every
non-genesis epoch (including a sequence-1 seal closing an empty epoch) and forbidden elsewhere;
key/body id binding uses the existing decode checks. A dying lane that observes the seal cannot
produce `(E, T+2)`: its state never advanced, so a state-derived next id retries `T+1` (r6 confirmed).
Cost: one seal per touched namespace per epoch transition, clean or unclean.

**INV-3 — the catalog with incarnations.** `cas/ref_catalog` (token-CAS like `gc/state`):

```text
namespace → { state: Creating | Live | Removing,
              incarnation: random 128-bit,
              creator: {server_root, fence_generation}   (Creating only) }
```

**Incarnations are chosen NOW, not deferred (r6-1):** the fresh-epoch-rebirth variant failed the
layout audit — the epoch allocator is server-root-wide and becomes the process epoch (no per-namespace
bump without a full remount), and verbatim files, fold cursors and cleanup work are qualified by
namespace only. Every namespace-scoped key family — ref logs, snapshots, `_ckpt`, verbatim files —
and every namespace-scoped state — fold-seal cursors, cleanup work items, cached namespace handles —
gains the incarnation qualifier; a cached handle whose incarnation mismatches the catalog is rejected
loudly. Rebirth = a new incarnation: old-life debris can never alias, physical-empty polling and the
`_cleanup` marker gate become removable — in that order (r6-11): incarnation wiring and the
`CasPartWriteTxn` recreation gate land first, `_cleanup` removal after. Capacity (r6-8): namespace
names get an explicit byte bound; the creation CAS itself verifies that the encoded catalog plus the
entry's worst-case lifetime cost (its `Removing` form plus its fold-seal cursor) fits both the catalog
and fold-seal caps — the check is atomic because it is computed against the exact body being CASed;
admission refuses loudly, removal is never refused.

**INV-4 — `_ckpt`, with a full state machine (r6-2).** `<ns>/<incarnation>/_ckpt`, token-CAS,
`{ life_epoch, checkpoint_snapshot_id | none, last_epoch_seal | none }` — forced by prefix cleaning
(with logs `1..100` deleted under a snapshot, `GET (E,1)`'s 404 cannot distinguish "cleaned" from
"empty"). Rules:

- **One update algorithm** for both writers (snapshot publisher; sealer): read → decode → validate
  `life_epoch`/incarnation → merge by SEMANTIC MAXIMUM per field, preserving the other field →
  token-CAS → on conflict reread and retry; on ambiguous CAS, reread and compare. Never write a stale
  body over a winner.
- **Snapshot retention:** a snapshot is deletable only STRICTLY BELOW `_ckpt.checkpoint`. Candidates
  at or above it are pinned (their publisher may still be about to CAS the pointer); they retire
  naturally when the checkpoint advances past them. This replaces newest-only-by-listing in the
  cleanup planner; a stale `_ckpt` can only under-clean.
- **Recovery base revalidation** (the 2026-07-11 restart rule, restored): if the sampled checkpoint
  snapshot is missing on exact read, reread `_ckpt`; token advanced → restart from the fresh
  authority; unchanged → corruption, fail closed.
- **Deletion ordering at removal:** exact-token-delete `_ckpt` while the catalog entry is still
  `Removing`; CAS-delete the catalog entry LAST. A successor `Creating` uses a fresh incarnation, so
  it cannot race the old life's cleanup.
- Strict enumeration classifies the checkpoint key explicitly (r6-11) — the first live `_ckpt` must
  not read as an unparseable key.

The append hot path touches neither the catalog nor `_ckpt`. LIST is a zero-trust hint everywhere.

## 3. Lifecycles {#lifecycles}

**Creation** (three conditional writes, DDL-rate): catalog CAS `Creating{incarnation, creator}` →
create `_ckpt` → catalog CAS `Live`. Reconciliation (r6-8): a stale `Creating` may be deleted only by
token-exact CAS and only after its CREATOR'S authority is terminal (that mount's lease expired or
fenced — a liveness fact the pool already tracks), never by wall-clock; a slow-but-live creator
cannot be raced. `dropNamespace` of a never-born entry deletes it directly.

**Removal:** admission = catalog CAS `Live → Removing` (the admission bound), then the terminal
`remove_namespace` record. **Only the owning mounted writer — or a successor that has claimed and
fenced that server root — may append the terminal record (r6-3):** GC never appends to a foreign
lane (the one-sequencer invariant); a `Removing` entry whose terminal record is missing is
surfaced by GC as a stuck-removal operator signal and completed by the owner/successor on its next
mount. After the terminal record folds: physical cleanup (incarnation-qualified, so omissions are
inert debris, not aliases) → `_ckpt` exact-delete → catalog entry delete.

**Recovery ownership (r6-4):** recovery captures the mount-fence GENERATION at admission; every
`slot-occupy`, `_ckpt` CAS and the final install require that same generation and a non-superseded
runtime; self-remount cancels or waits out `recovery_in_progress` before rearming the fence. Two
recoveries of one namespace can never interleave their writes.

## 4. Recovery {#recovery}

Catalog (state + incarnation) → `_ckpt` (checkpoint + last seal) → exact-key snapshot (revalidation
rule above) → arithmetic tail (`last + 1`; hint omissions fetched by exact key; a 404 below a durable
same-epoch higher id is impossible-by-INV-1 → vanish-restart, then fail closed) → CAS-walk + seal →
`_ckpt` CAS → install. Acked ⇒ durable ⇒ dense ⇒ found.

## 5. GC fold {#fold}

One catalog `GET`; ONE strict hint enumeration (feeds intake, cleanup planning, defer); per namespace
with hinted candidates (quiet = zero cost): advance by arithmetic (`cursor + 1`, `GET` owed anyway —
hint holes including the observed `0x1430c`/`0x1430d` shape are folded through without noticing);
cross epochs only by consuming seals (`prev_epoch_seal` must name the consumed seal); frontier =
expected-next absent; impossible shapes → per-namespace hold, loud, `suppress_destructive` pool-wide
while carried, backoff, operator surface. Whole-round abort only for a key unattributable to any
namespace. Cursors are keyed by catalog entries (namespace + incarnation); unhinted namespaces carry
verbatim; retirement only with catalog-entry deletion. B1: `logs_accounted == logs_applied` over the
cut, `EpochSeal` an applied no-op (B2 `produced=false`). **Probe A** stays as a sampled store-quality
detector with a DETERMINISTIC cadence and durable `sample due/performed/skipped` observability
(r6-12); it aborts nothing and does not replace the mount-time store gate (#23). Cleanup: covered
logs are contiguous computable ranges under `_ckpt.checkpoint` + cursor; crossed dead epochs delete
as closed ranges.

## 6. Orphan-manifest sweep {#sweep}

Grants stay in their build's epoch; removals cross epochs. Rules:

- a manifest of an epoch-`E` build is deletable only when the cursor consumed epoch `E`'s seal AND no
  unconsumed tail record names it as a removal target (the normative S42 rule). **Bounded execution
  (r6-6):** the tail protection set is built ONCE per namespace per round by the arithmetic walk the
  fold already performs, shared across every candidate of that namespace, and budgeted under the
  common round budget; if the walk cannot reach the frontier within budget, every affected candidate
  is retained and the scan resumes next round from the fold cursor. No per-candidate rescans.
- live-epoch bodies: the mount retries its cleanup duties from an in-memory queue while it lives —
  **stated honestly, not called durable (r6-5)** — and crash remnants are covered by the successor
  path: the next epoch's first touch seals the old epoch, which makes the dead builds' manifests
  sweep-eligible under the rules above; a build is not retired while an owner-grant outcome is
  uncertain (INV-1's wedge rule).
- **orphan BLOBS (r6-9):** deleting a proven-dead orphan manifest nominates its blobs for a
  current-edge-count recheck through the normal condemn pipeline (exact-token discipline unchanged) —
  this closes the "sweep deletes the manifest, blobs have no in-degree row, nothing ever condemns
  them" leak. Blobs uploaded with NO manifest ever are a NAMED residual leak requiring a future
  build/upload registry; REBUILD does not hide it (§7).

Lands together with the S42 stale-edge fix as one coherent sweep change.

## 7. REBUILD and fsck {#rebuild-fsck}

REBUILD rebuilds cursors and edges from catalog + `_ckpt` + arithmetic tails and **condemns nothing**
(a hidden live manifest plus a listed blob must never condemn acked data). It is documented as
logical-edge repair: orphan-blob reclamation happens through §6's nomination path, not through
REBUILD; manifest-less blobs are the named residual. fsck: universe from the catalog, streams by
arithmetic (`chain-broken` fatal in summary AND exit code), tails above `_ckpt.checkpoint`,
`unchecked` reserved for the genuinely unproven; a healthy pool returns clean.

## 8. Performance {#performance}

Measured base unchanged (3.42 M serial round trips per 30-min round; 256 logs/s; 39.6 %
cross-transaction manifest re-reads; `pending_deletes` 77.2 s). Gains: P1 prefetch by arithmetic
(mispredictions impossible); ONE strict enumeration per round (probe A's second walk only on sampled
rounds); range cleanup; fsck tails. Unchanged: P2 cache; HEAD-per-edge veto; the rig (#10). Future
lever: snapshot-diff folding (BACKLOG). Honest costs (r6-13): append +0 requests; creation = THREE
conditional writes; `_ckpt` updates may loop on CAS conflicts; recovery +1 conditional PUT per touched
namespace per transition (+1 per adopted straggler) + one `_ckpt` CAS; snapshot publication +1 `_ckpt`
CAS (async); fold +1 catalog `GET` per round, optional frontier probe per active namespace; the
sweep's tail walk shares the fold's round budget (dominates its cost model on deep backlogs);
verbatim access carries the cached incarnation (rejected loudly on mismatch). Formats: `EpochSeal`,
`prev_epoch_seal`, catalog, `_ckpt`, incarnation qualifiers; DELETED: pool-wide sequence, synthetic
snapshot ids, `sealed_from`, listing-based snapshot discovery, `_cleanup` markers (after incarnation
wiring lands). Pre-release; no compat scaffolding.

## 9. What this deletes {#deletions}

From v1–v4: the entire certificate stack (prev links, seal intervals, `NeverBorn`, seal pointer,
birth authority, recovery generation, `R*`, admission object, tombstones, sticky floors, pins). From
current code: pool-wide `next_ref_sequence` and safe-gap reasoning; synthetic recovery-seal
snapshots; `snapshots.back()` discovery in recovery AND cleanup planning; the second enumeration as a
correctness dependency; carry-forward as a special case; `_cleanup` markers and physical-empty
rebirth gating (AFTER incarnation wiring, in that order).

## 10. Verification {#verification}

All tests RED first, as fault-injected interleavings with assertions that fail before the protocol
change (r6-13), on `HoleyListBackend` + a delayed-PUT fault backend:

- arithmetic fold-through of hidden middles; 404-below-witness → hold; frontier semantics;
- every-attempt rule: ambiguous-then-definite keeps the wedge; the delayed attempt lands and is
  adopted; the all-ambiguous wedge's same-bytes retry resolves both ways; `NoAttemptSent` still frees;
- CAS-walk both directions through `slot-occupy`; adopted seal terminates; clean-transition sealing;
  the dying lane retries `T+1`, never `T+2`; seal grammar (solo op; `prev_epoch_seal` exactly on
  sequence 1, including an empty-epoch seal);
- `_ckpt`: cleanup raced precisely between snapshot PUT and pointer CAS → pinned, no dangling
  pointer; stale sampled base → authority reread/restart/corruption three-way; merge preserves the
  other field under concurrent sealer/publisher; exact-delete ordering at removal;
- recovery paused across a self-remount and rearmed → old generation's writes refused (r6-4);
- incarnations: rebirth with the same logical verbatim name + a stale old-life handle → rejected;
  stale-handle fold/cleanup attempts rejected; `Creating` reconciliation cannot race a slow live
  creator (fence-terminal gating); capacity refusal at creation, never at removal;
- removal: GC never appends (the stuck-`Removing` surface fires instead); owner/successor completes;
- sweep: removal-tail retention under an exhausted budget retains and resumes; kill after cleanup
  enqueue → successor seal + sweep reclaims manifests AND nominated blobs; manifest-less blob leak
  is visible in fsck's accounting, not silently green;
- REBUILD: asymmetric manifest/blob omission condemns nothing; orphan-blob progress verified through
  the nomination path;
- catalog/`_ckpt` absence → fail closed loudly; probe A's sampling cadence deterministic and
  observable; B1 identity with `EpochSeal` no-ops; fsck clean on healthy, fatal on `chain-broken`.

**TLA+ is phase 0 of the plan** (per project precedent: model changes land and go green BEFORE code),
against the existing `docs/superpowers/models/` suite and its conventions (green configs, `_sab_*`
sabotage configs proving each property can go red, `run_*.sh` runners, `*_RESULTS.md`):

| model | v7 impact |
|---|---|
| `CaRefTableSnapshotLogCore` | REWRITE core: INV-1 allocator (state-derived next id; every-attempt reuse; ambiguous-then-definite wedge + same-bytes retry), INV-2 CAS-walk/`EpochSeal`/`prev_epoch_seal`, INV-4 `_ckpt` as the recovery base (merge loop, strictly-below retention, revalidation). `LatePredecessorPut` FLIPS from counterexample to proof: no record lands behind a closed frontier. |
| `CaRefDeltaIntakeCore` | REWRITE premises: enumerate-once/list-trust replaced by arithmetic advance with hint-only listings; epoch crossing by seal consumption; sabotage = arbitrary hint omission (incl. a wholly-hidden epoch) must be unable to move the cursor. |
| `CaRelinkConfirmCore` (`_sab_holeylist`) | The defect mechanization becomes the fix's permanent regression witness: the sabotaged enumeration can no longer reach the damage state under v7 intake. |
| `CaRefNsCleanupStaleLeaderCore` | REWRITE around catalog states + incarnations replacing the `_cleanup` gate; stale-leader straggler deletes become inert-by-incarnation (the property turns structural); removal-append legality (owner/fenced successor only, never GC). |
| `CaRefWriterCleanupCore` | Extend: build-not-retired-under-uncertainty; cleanup duties; the grant/removal epoch-locality facts the sweep rules rest on. |
| `CaRefFoldClampRecoveryCore` (or sibling `CaSweepEpochSealCore`) | Extend: sweep eligibility = epoch seal consumed AND no unconsumed removal names the manifest (the S42 rule); orphan-blob nomination entering the condemn pipeline without breaking two-phase/exact-token. |
| `CaCasMountCore` | Extend its fence sabotage family: recovery generations captured at admission; `slot-occupy`/`_ckpt`/install refused across a remount. |
| `CaRefCatalogCore` (NEW) | Catalog lifecycle (`Creating` reconciliation bound to creator fence-liveness; capacity at the admission CAS; `Removing` completion), `_ckpt` deletion ordering, incarnation aliasing properties (no stale handle crosses incarnations; no dangling checkpoint; stale `_ckpt` under-cleans only). Reuses `CaIncarnationCore`'s vocabulary. |
| `CaErasureProof`, `CaDiskLifecycle`, ack-floor/condemn family | AUDIT: they reference listing behavior; verify none encodes LIST-trust v7 removes, and that condemn-entry admits the nomination path. |

Then consult round 7, then the soak gate.

## 11. History {#history}

Contiguity is the project's own I7 resurrected (2026-07-10 spec; died as collateral of the
RefSnapLog-era model, never refuted). The ghost was documented as `Late Predecessor PUT` (rev.4) with
closure deferred on the "no extra request per ordinary mutation" constraint — which INV-2 honors.
Rounds 1–4 rejected the certificate stack and discovered the facts that shaped the invariants; the
blinded consult independently reproduced the diagnosis and contributed the catalog, the checkpoint
(as the off-hot-path `_ckpt`) and the incarnation scheme (adopted in v7 after the fresh-epoch variant
failed round 6's layout audit: the epoch allocator is server-root-wide; verbatim files and cursor
state are namespace-only). Round 5 validated the v5 skeleton and forced `_ckpt`, the every-attempt
rule, and the sweep/REBUILD repairs. Round 6 validated those directions ("checks that held":
state-derived next-id cannot overshoot a seal; seal-on-every-transition; S42 rule safety) and forced
the v7 algorithms: the `_ckpt` state machine, incarnation wiring order, sequencer-legal removal
reconciliation, recovery generations, wedge liveness, bounded tail scans, catalog reconciliation
bound to creator liveness, the orphan-blob nomination path, and the seal grammar.

## 12. Alternatives {#alternatives}

| alternative | disposition |
|---|---|
| v1–v4 certificate stack | Rejected by the user as accretion; deleted. |
| Full head-CAS commit chain (blinded consult) | North star if the wedge is ever worth deleting; not taken: moves the commit point (+1 CAS per flush — rev.4's refused cost), rewrites the hardest module, needs LIST-hints for discovery anyway. v7 carries its catalog, checkpoint and incarnations. |
| Fresh-epoch rebirth (v6 §3) | **Failed round 6's audit**: the epoch allocator is server-root-wide (no per-namespace bump without full remount); verbatim files, fold cursors and cleanup work are unqualified. Superseded by catalog incarnations. |
| Checkpoint inside the catalog | Rejected: pool-wide CAS contention at snapshot-publication rate. |
| Never cleaning covered logs | Rejected: unbounded storage; `_ckpt` is the price and it is small. |
| RefSnapLog combined mutable state; local floors; enforced-timing ghost containment; widened probe A | Rejected earlier (perf history; user ruling; round-3 refutation; permanent-block risk). |

## 13. Out of scope, named {#out-of-scope}

The mount-time LIST probe (#23); decommission proven-dead fencing; snapshot-diff folding; P1/P2/P3
and the rig (#10); the 56 leaked blobs; the `-1`-before-`+1` path; the RustFS mechanism; the
build/upload registry that would close the manifest-less-blob residual and enable a condemning
REBUILD.
