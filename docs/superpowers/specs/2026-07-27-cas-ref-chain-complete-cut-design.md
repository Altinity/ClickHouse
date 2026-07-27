---
description: 'v8 design for the LIST-incompleteness release blocker: per-namespace contiguous ref ids, in-band epoch seals, a namespace catalog with incarnations, a per-namespace checkpoint object, and a mandatory per-namespace frontier proof on destructive rounds — LIST is a zero-trust hint; two new object kinds, both off the append hot path.'
sidebar_label: 'CAS ref contiguous-chain'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: contiguous ref streams and the in-band epoch seal'
doc_type: 'reference'
---

# CAS: contiguous ref streams and the in-band epoch seal {#cas-ref-contiguous-chain}

**Date:** 2026-07-28. **Status:** v8 — v7 plus the round-7 resolutions (mandatory destructive-round
frontier proof; durable hold carrier; nomination atomicity; handle protocol; aggregate capacity;
recreate-only migration); awaiting review round 8 and user review. **Branch:** `cas-gc-rebuild`.

Fixes BACKLOG `{#list-as-journal-dataloss-2026-07-25}` (observed:
`reports/2026-07-26-list-incompleteness-investigation.md`) and closes the rev.4 `Late Predecessor
PUT` limitation. Realizes P4 of `cas/draft-fixes-20260726.md`: zero added requests on the append path
and on the fold's per-record path. Two new object kinds (`ref_catalog`, `_ckpt`), both off the append
hot path.

## 1. Problem and shape {#problem}

GC folds what a listing returned and seals a cursor above what it OBSERVED; a hidden `-1` is a
permanent leak, a hidden `+1` deletes acked data. Recovery, the orphan sweep, REBUILD and fsck consume
the same untrusted listings. Root cause (converged on independently twice): **absence is undecidable
in a sparse id space** — the pool-wide `next_ref_sequence` makes per-namespace gaps the norm, so every
hole demanded another certificate (the rejected v1–v4 stack, §12). v8's invariants make absence and
committed-ness decidable from arithmetic, point reads, and conditional writes — the operations the
store performed honestly even while its LIST lied. One consequence is stated up front (round-7
blocker 1): because blob in-degree is POOL-WIDE, per-namespace cursor immobility is not deletion
safety — a destructive round must hold a frontier proof for EVERY catalog namespace, so a quiet
namespace costs one exact 404 `GET` per destructive round. That is the honest price of pool-wide
deletion soundness; at extreme namespace counts it is the knob where the head-CAS alternative (§12)
re-enters.

## 2. Invariants {#invariants}

**INV-1 — per-namespace contiguous ids.** Next id = `greatest_applied.ref_sequence + 1` (already
computed as `commitRefChunk`'s trial preview); the pool-wide atomic is deleted; within
`(namespace, epoch)` durable ids are dense `1..T`. **Reuse only under the every-attempt rule**: an id
is freed only when nothing was sent or every sent attempt has its own conclusive rejection; a
definite rejection AFTER an ambiguous attempt keeps the outcome `Unresolved` and the lane wedged.
**Wedge liveness, enveloped (r7-8):** the wedge stores the fence generation captured at admission
(`RefAppendWedge` gains that field); each FOREGROUND lane flush performs at most one normal bounded
controller operation — a same-`(key, bytes)` conditional create under that generation — and an
unresolved result retains the wedge for the next caller or remount. No background retry loop resets
`operation_deadline_ms`. A successor's valid `EpochSeal` found at the key is a CONCLUSIVE rejection
of the old transaction, not corruption. Remount remains the backstop.

**INV-2 — every epoch transition is closed in-band.** First touch after ANY writer-epoch transition
CAS-walks the dead epoch's tail via the dedicated **`slot-occupy`** primitive
(`Created | Occupied(bytes, token) | Unresolved`). `Occupied` → adopt and replay (a straggler, or an
`EpochSeal`, which terminates the walk); `Created` → the seal occupies `(E, T+1)` and the ghost can
never land — the store's conditional create is the fence. `EpochSeal` grammar: exactly one seal
operation per transaction, no edges, no lifecycle ops; `prev_epoch_seal` REQUIRED on exactly
sequence 1 of every non-genesis epoch (including a sequence-1 seal closing an empty epoch) and
forbidden elsewhere. A dying lane that observes the seal cannot produce `(E, T+2)` — a state-derived
next id retries `T+1`. Cost: one seal per touched namespace per epoch transition.

**INV-3 — the catalog with incarnations.** `cas/ref_catalog`, token-CAS:
`namespace → {state: Creating | Live | Removing, incarnation, creator: {server_root,
fence_generation}}`. Every namespace-scoped KEY (ref logs, snapshots, `_ckpt`, verbatim files) is
incarnation-qualified with a canonical right-to-left grammar `<ns>/<incarnation>/{_log,_snap,_ckpt}`
+ explicit refusal of legacy-shaped keys (r7-10); every namespace-scoped STATE (fold cursors,
cleanup work, HOLD records) is keyed by `(namespace, incarnation)`. Manifests keep their existing
`(namespace, mount-epoch, build-sequence)` identity — build ids are mount-global and monotone, so a
reborn namespace's new builds cannot alias old-life manifests; the residual obligation (a deposed
old-life worker must not enumerate new-life manifests) is carried by the handle protocol below and
proven in the plan (r7-4, the exclusion arm). **Handle protocol (r7-4):** one typed
`NamespaceHandle{namespace, incarnation, local_life_generation}` threaded through `Pool`,
`CasPlainObjects`, `PartWriteTxn`, ref recovery/publishing, cleanup and the metadata-storage
operations above them; `Pool` maintains a cached catalog snapshot; LOCAL lifecycle transitions
invalidate the shared life token synchronously, and REMOTELY initiated transitions are safe because
they require the old mount's fence to be terminal first — so hot reads check a local generation, not
the catalog. **Aggregate capacity (r7-5):** the creation CAS verifies the additive predicate —
`encoded_catalog(post) ≤ catalog_cap` AND `fold_fixed + Σ reserve_cursor + Σ reserve_removing_cleanup
+ Σ reserve_hold ≤ fold_seal_cap` over ALL entries — computable at admission because namespace
length, tokens, reason strings and record counts all have fixed maxima; the catalog itself is the
serialized admission ledger. Admission refuses loudly; removal is never refused.

**INV-4 — `_ckpt`.** `<ns>/<incarnation>/_ckpt`, token-CAS,
`{life_epoch, checkpoint_snapshot_id | none, last_epoch_seal | none}` — forced by prefix cleaning.
One update algorithm for both writers: read → validate → merge by semantic maximum per field →
token-CAS; **a merge producing the exact current body returns WITHOUT a CAS** (no token churn), and
the retry/restart loop is bound to the existing recovery deadline and bounded-restart accounting
(r7-9). Snapshots are deletable only STRICTLY BELOW `_ckpt.checkpoint`. Missing sampled base →
reread `_ckpt`: token advanced → restart; unchanged → corruption, fail closed. Removal deletes
`_ckpt` by exact token while the catalog entry is still `Removing`, catalog entry last.

## 3. Lifecycles {#lifecycles}

**Creation** (three conditional writes, DDL-rate): catalog `Creating{incarnation, creator}` →
`_ckpt` create → catalog `Live`. A stale `Creating` is reconciled only by token-exact CAS and only
after its creator's fence is terminal. **Removal:** catalog `Live → Removing` (admission), then the
terminal record — appended ONLY by the owning mounted writer or a successor that has claimed and
fenced that server root; GC surfaces stuck removals, never appends. **Decommission is a named
dependency (r7-6):** `2026-07-13-cas-pool-member-decommission-design.md` is updated in the same
rollout — after claiming the victim, the decommission actor enumerates the victim's catalog entries
EXACTLY (never by LIST), treats `Removing` without a terminal record as resumable writer work, and
slot retirement is forbidden while any catalog entry owned by that server root remains. **Recovery
ownership:** the mount-fence generation is captured at admission and required on every `slot-occupy`,
`_ckpt` CAS and install; self-remount cancels or waits out `recovery_in_progress` before rearming.

**Migration (r7-7): there is no in-place migration.** The pool format is bumped; startup against the
old format fails closed with a message that names pool recreation. Existing pre-release/soak pools
are recreated (`down -v` is already their hygiene). Bootstrapping catalog/`_ckpt` from the old
format's listings would rest on the exact trust this design removes; preserving a pool would be a
separate offline migration protocol, deliberately not built.

## 4. Recovery {#recovery}

Catalog (state + incarnation) → `_ckpt` (checkpoint + last seal) → exact-key snapshot (revalidation
rule) → arithmetic tail (`last + 1`; hint omissions fetched by exact key; a 404 below a durable
same-epoch higher id → vanish-restart, then fail closed) → CAS-walk + seal → `_ckpt` CAS → install.
Acked ⇒ durable ⇒ dense ⇒ found.

## 5. GC fold {#fold}

Per round: one catalog `GET`; ONE strict hint enumeration (intake, cleanup planning, defer). Fold
work runs per namespace with hinted candidates, advancing by arithmetic (`cursor + 1`, `GET` owed
anyway; hint holes — including the observed `0x1430c`/`0x1430d` shape — are folded through without
noticing); epochs are crossed only by consuming seals; impossible shapes (a 404 below a same-epoch
witness, an unconsumed-seal crossing) HOLD the namespace.

**The destructive-round frontier proof (r7-1, load-bearing).** Blob in-degree is pool-wide, so a
round may run DESTRUCTIVE work (condemnation, graduation, deletion, sweep deletes) only if it holds a
frontier proof for EVERY `Live`/`Removing` catalog entry: hinted-active namespaces prove theirs by
walking to an absent expected-next; every other namespace gets one exact `GET cursor+1` — present →
it was wrongly quiet, walk it; absent → frontier proven. If the round budget expires before every
namespace has a proof, cursor advances may still seal, but ALL destructive work is suppressed that
round. Fold-only rounds may skip unhinted namespaces entirely (delay is safe when nothing deletes).
Cost: one 404 `GET` per quiet namespace per destructive round, parallelizable under the shared
budget; at extreme namespace counts this is the scaling knob where the head-CAS alternative re-enters
(§12).

**The durable hold carrier (r7-2).** `ShardCoverage::classification == 4` IS the hold state,
persisted and carried in the fold seal with `{reason code, retry/backoff state}` per
`(namespace, incarnation)`. `suppress_destructive` is computed as the OR of the current round's
anomalies AND every CARRIED hold; a carried hold forces an exact retry of its offending key even
when the hint omits the namespace, and clears only on a successful frontier walk or an explicit
operator repair. A hold therefore can never be forgotten by a listing.

Cursors are keyed by catalog entries; unhinted namespaces carry verbatim (the catalog-keyed rule —
what §9 deletes is the LISTING-keyed carry-forward special case). B1:
`logs_accounted == logs_applied` over the cut, `EpochSeal` an applied no-op (B2 `produced=false`).
Probe A: sampled, deterministic cadence, durable due/performed/skipped observability; aborts
nothing; does not replace the mount-time store gate (#23). Cleanup: covered logs are contiguous
computable ranges under `_ckpt.checkpoint` + cursor; crossed dead epochs delete as closed ranges.
Whole-round abort only for a key unattributable to any namespace.

## 6. Orphan-manifest sweep {#sweep}

Grants stay in their build's epoch; removals cross epochs. Rules:

- a manifest of an epoch-`E` build is deletable only when the cursor consumed epoch `E`'s seal AND
  no unconsumed tail record names it as a removal target (the normative S42 rule); the tail
  protection set is built once per namespace per round under the common budget, shared across
  candidates, resumed from the fold cursor (sound: removals at or below the adopted cursor have
  folded); budget exhausted → retain every candidate of that namespace;
- **nomination is failure-atomic (r7-3):** exact-`GET` and decode manifest `M` FIRST; feed its
  `BlobRef`s through a dedicated neutral nomination input — a touch-and-current-count recheck that
  bypasses B2 ordinals and unmatched-remove accounting (a synthetic `BlobDelta` would corrupt both);
  adopt the nominations in the round's `gc/state` CAS; only THEN exact-token-delete `M`. A lost CAS
  leaves `M` rediscoverable; death after adoption retries the delete idempotently;
- live-epoch bodies: the mount retries its cleanup duties in memory while it lives (stated honestly,
  not called durable); crash remnants are covered by the successor's epoch seal, which makes the dead
  builds' manifests sweep-eligible; the guaranteed adopter for never-touched namespaces of a dead
  member is the decommission actor (§3's named dependency);
- manifest-less blobs remain the NAMED residual leak requiring a future build/upload registry.

Lands together with the S42 stale-edge fix as one coherent sweep change.

## 7. REBUILD and fsck {#rebuild-fsck}

REBUILD rebuilds cursors and edges from catalog + `_ckpt` + arithmetic tails and **condemns
nothing** — orphan-blob reclamation flows through §6's nomination path. fsck: universe from the
catalog, streams by arithmetic (`chain-broken` fatal in summary AND exit code), tails above
`_ckpt.checkpoint`, `unchecked` reserved for the genuinely unproven; a healthy pool returns clean.

## 8. Performance {#performance}

Measured base unchanged (3.42 M serial round trips per 30-min round; 256 logs/s; 39.6 %
cross-transaction manifest re-reads; `pending_deletes` 77.2 s). Gains: P1 prefetch by arithmetic;
ONE strict enumeration per round; range cleanup; fsck tails. Unchanged: P2 cache; HEAD-per-edge veto;
the rig (#10). Future lever: snapshot-diff folding (BACKLOG). Honest costs: append +0 requests;
creation = three conditional writes; `_ckpt` may loop on CAS conflicts (bounded, no-op writes
skipped); recovery +1 conditional PUT per touched namespace per transition (+1 per adopted
straggler) + one `_ckpt` CAS; snapshot publication +1 `_ckpt` CAS; fold +1 catalog `GET` per round;
**destructive rounds add one exact `GET` per quiet namespace** (r7-1 — the frontier proof; the
former "quiet = zero" claim was wrong for deletion safety); deep arithmetic walks dominate backlog
cost and share the round budget with the sweep's tail scan; verbatim access carries the cached
handle (no hot-path catalog read). Formats: `EpochSeal`, `prev_epoch_seal`, catalog, `_ckpt`,
incarnation-qualified key grammar; DELETED: pool-wide sequence, synthetic snapshot ids,
`sealed_from`, listing-based snapshot discovery, `_cleanup` markers (after incarnation wiring).
Pre-release; recreate-only migration (§3).

## 9. What this deletes {#deletions}

From v1–v4: the entire certificate stack (prev links, seal intervals, `NeverBorn`, seal pointer,
birth authority, recovery generation, `R*`, admission object, tombstones, sticky floors, pins). From
current code: pool-wide `next_ref_sequence` and safe-gap reasoning; synthetic recovery-seal
snapshots; `snapshots.back()` discovery in recovery AND cleanup planning; the second enumeration as
a correctness dependency; the LISTING-keyed carry-forward special case (the catalog-keyed carry
replaces it); `_cleanup` markers and physical-empty rebirth gating (after incarnation wiring, in
that order).

## 10. Verification {#verification}

All tests RED first, as fault-injected interleavings on `HoleyListBackend` + a delayed-PUT fault
backend. The round-7 controls join the list:

- **the cross-namespace scenario:** wholly hidden `A:+1` (acked) + visible `B:-1` on the same blob →
  without the frontier proof the blob dies (control goes red), with it the round suppresses;
- carried hold followed by a round whose hint omits the held namespace → suppression persists, the
  offending key is retried;
- nomination: death before adoption leaves `M` rediscoverable; death after adoption retries the
  delete; nominations never touch B2/unmatched accounting;
- serialized catalog admissions that jointly exceed the aggregate fold reservation → refused at the
  CAS;
- decommission with a `Removing` entry the victim's LIST hides → completed via exact catalog
  enumeration; slot retirement refused while entries remain;
- opening a legacy-format pool → fails closed naming recreation;
- stale-handle rebirth between `namespaceFilesReadable` and the verbatim operation → rejected by the
  life token;
- plus the v7 set: arithmetic fold-through of hidden middles; 404-below-witness holds; every-attempt
  wedge incl. same-bytes retry envelope and successor-seal adoption; CAS-walk both directions;
  clean-transition sealing; `T+1` retry never `T+2`; seal grammar; `_ckpt` races (cleanup between
  PUT and CAS; stale base three-way; merge preserves fields; no-op skip); recovery across remount;
  incarnation aliasing incl. verbatim; `Creating`/`Removing` reconciliation; capacity refusal;
  removal-tail retention resume; probe-A cadence; B1 with seal no-ops; fsck clean/fatal both ways.

**TLA+ is phase 0 of the plan** (models land green before code), against `docs/superpowers/models/`
conventions (green + `_sab_*` configs proving each property can go red, `run_*.sh`, `*_RESULTS.md`):

| model | v8 impact |
|---|---|
| `CaRefTableSnapshotLogCore` | REWRITE core: INV-1 allocator (state-derived id; every-attempt reuse; wedge envelope), INV-2 CAS-walk/`EpochSeal`/`prev_epoch_seal`, INV-4 `_ckpt` as recovery base. `LatePredecessorPut` FLIPS from counterexample to proof. |
| `CaRefDeltaIntakeCore` | REWRITE premises: arithmetic advance with hint-only listings; epoch crossing by seal consumption; **the destructive-round frontier proof and the cross-namespace hidden-`+1` sabotage** (r7-1's scenario must be unable to delete); the durable hold carrier feeding suppression. |
| `CaRelinkConfirmCore` (`_sab_holeylist`) | The defect mechanization becomes the fix's permanent regression witness. |
| `CaRefNsCleanupStaleLeaderCore` | REWRITE around catalog states + incarnations replacing the `_cleanup` gate; straggler deletes inert-by-incarnation; removal-append legality (owner/fenced successor only). |
| `CaRefWriterCleanupCore` | Extend: build-not-retired-under-uncertainty; cleanup duties; grant/removal epoch-locality. |
| `CaRefFoldClampRecoveryCore` (or sibling `CaSweepEpochSealCore`) | Extend: sweep eligibility (seal consumed AND no unconsumed removal), the failure-atomic nomination ordering, neutral recheck input. |
| `CaCasMountCore` | Extend fence sabotage family: recovery generations on `slot-occupy`/`_ckpt`/install; the wedge-retry vs successor-seal race. |
| `CaRefCatalogCore` (NEW) | Catalog lifecycle (creator-fence reconciliation; the ADDITIVE capacity predicate; `Removing` completion incl. the decommission adopter), `_ckpt` deletion ordering, incarnation aliasing, no-dangling-checkpoint, under-clean-only. Reuses `CaIncarnationCore` vocabulary. |
| `CaErasureProof`, `CaDiskLifecycle`, ack-floor/condemn family | AUDIT: no residual LIST-trust; condemn-entry admits the neutral nomination input. |

Then consult round 8, then the soak gate.

## 11. History {#history}

Contiguity is the project's own I7 resurrected (2026-07-10 spec); the ghost was documented as `Late
Predecessor PUT` (rev.4) with closure deferred on the "no extra request per ordinary mutation"
constraint — honored by INV-2. Rounds 1–4 rejected the certificate stack; the blinded consult
independently reproduced the diagnosis and contributed the catalog, the checkpoint and the
incarnation scheme. Round 5 validated the v5 skeleton and forced `_ckpt`, the every-attempt rule and
the sweep/REBUILD repairs. Round 6 held the invariants and forced the v7 algorithms. Round 7
(dispositions: 6 resolved / 7 partial / 0 unresolved of round 6) found the two remaining data-loss
scenarios — the pool-wide in-degree hole behind "quiet = zero cost" and the undurable hold — plus
the nomination atomicity, handle, capacity, decommission, migration and wedge-envelope gaps; v8
folds all ten in, at the honest cost of one exact `GET` per quiet namespace per destructive round.

## 12. Alternatives {#alternatives}

| alternative | disposition |
|---|---|
| v1–v4 certificate stack | Rejected by the user as accretion; deleted. |
| Full head-CAS commit chain (blinded consult) | North star. Not taken: moves the commit point (+1 CAS per flush — rev.4's refused cost) and rewrites the hardest module. v8 carries its catalog, checkpoint, incarnations — and notes that at extreme namespace counts the destructive-round frontier sweep (one `GET` per namespace) approaches the head design's read cost, which is where this trade should be revisited. |
| Fresh-epoch rebirth | Failed round 6's audit (server-root-wide allocator; unqualified verbatim/cursor state). Superseded by incarnations. |
| Checkpoint inside the catalog | Rejected: pool-wide CAS contention at snapshot-publication rate. |
| Never cleaning covered logs | Rejected: unbounded storage. |
| In-place migration of legacy pools | Rejected (r7-7): bootstrapping the new authorities from old listings rests on the trust this design removes; recreate-only. |
| RefSnapLog combined mutable state; local floors; enforced-timing ghost containment; widened probe A | Rejected earlier (perf history; user ruling; round-3 refutation; permanent-block risk). |

## 13. Out of scope, named {#out-of-scope}

The mount-time LIST probe (#23); snapshot-diff folding; P1/P2/P3 and the rig (#10); the 56 leaked
blobs; the `-1`-before-`+1` path; the RustFS mechanism; the build/upload registry (manifest-less
blob residual; condemning REBUILD). The pool-member decommission spec is NOT out of scope as a
dependency: its catalog-exact enumeration and slot-retirement rules land in the same rollout (§3).
