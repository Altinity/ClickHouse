---
description: 'v6 design for the LIST-incompleteness release blocker: per-namespace contiguous ref ids, in-band epoch seals that CAS-occupy the next slot, a point-readable namespace catalog, and a per-namespace checkpoint object — LIST is a zero-trust hint everywhere, and exactly two new object kinds exist.'
sidebar_label: 'CAS ref contiguous-chain'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: contiguous ref streams and the in-band epoch seal'
doc_type: 'reference'
---

# CAS: contiguous ref streams and the in-band epoch seal {#cas-ref-contiguous-chain}

**Date:** 2026-07-28. **Status:** v6 — v5's invariants plus the ten round-5 resolutions; awaiting
adversarial review round 6 and user review. **Branch:** `cas-gc-rebuild`.

Fixes BACKLOG `{#list-as-journal-dataloss-2026-07-25}` — observed, not modelled
(`reports/2026-07-26-list-incompleteness-investigation.md`) — and closes the `Late Predecessor PUT`
limitation documented in the Phase-1 ref spec (rev.4). Realizes P4 of `cas/draft-fixes-20260726.md`
within its refutation condition: zero added requests on the append path and on the fold's per-record
path. **Exactly two new object kinds exist** (`ref_catalog`, `_ckpt`), both off the append hot path.

## 1. Problem and shape of the solution {#problem}

GC discovers ref-log transactions by listing `cas/refs/`, folds what the listing returned, and seals a
per-namespace cursor above what it OBSERVED, not what EXISTS: a missed `-1` is a permanent leak, a
missed `+1` lets GC delete a blob a committed manifest still references. Writer recovery, the orphan
sweep, REBUILD and fsck consume the same untrusted listings.

Root cause, converged on independently by this author and a blinded consult: **absence is undecidable
in a sparse id space** (`next_ref_sequence` is pool-wide, so per-namespace gaps are the norm), and
committed-ness is inferable only from enumeration. v1–v4 tried to certify absence and accreted a
rejected machinery stack (§13). v6 instead changes the invariants so absence and committed-ness are
decidable from arithmetic, point reads, and conditional writes — the operations the store performed
honestly even while its LIST lied.

## 2. The invariants {#invariants}

**INV-1 — per-namespace contiguous ids.** `ref_sequence` becomes per-namespace: next id =
`greatest_applied.ref_sequence + 1` (the value `commitRefChunk` already computes as its trial preview).
The pool-wide atomic is deleted. Within `(namespace, epoch)`, durable ids are dense `1..T`: no gaps,
ever. **Id reuse is permitted only when non-application is proven for EVERY attempt**: nothing was sent
(`NoAttemptSent`, pre-PUT encode/validation failures), or each sent attempt has its own conclusive
rejection. A definite rejection AFTER an earlier ambiguous attempt does NOT free the id — the earlier
attempt may still land — so the outcome stays `Unresolved` and the existing wedge holds the lane until
exact-key occupancy resolves it (round-5 blocker 2; the 2026-07-11 spec's own rule, restored). The
wedge contract is otherwise untouched: at most one id per namespace is ever in flight.

**INV-2 — every epoch transition is closed in-band.** On the first touch of a namespace after ANY
writer-epoch transition — clean or unclean — recovery finds the dead epoch's tail with a **CAS-walk**:
`slot-occupy` the next expected slot `(E, k+1)`.

- **Occupied** → adopt the returned bytes (a landed straggler or a hint-hidden acked record — decoded
  as `RefLogTxn` or as an already-present `EpochSeal`, which terminates the walk), replay, `k+1`,
  repeat. Bounded: at most one in-flight record (wedge) plus hint omissions, each iteration consumes
  real data.
- **Created** → the `EpochSeal` record now occupies `(E, k+1)`: epoch `E` is closed at `k`, and the
  `Late Predecessor PUT` ghost can never land — its only possible slot is taken; the store's own
  conditional create is the fence. No timing argument anywhere.

`slot-occupy` is a NEW controller primitive returning `Created | Occupied(bytes, token) | Unresolved`
— the existing immutable-object controller throws `CORRUPTED_DATA` on foreign bytes and must not be
bent to this job (round-5 major 7). The dying lane's view of the same race keeps today's conclusive
different-object classification; the predecessor is fenced regardless. The `EpochSeal` is an ordinary
ref-log record (new op kind), folded as a state no-op; the first record of the next epoch carries
`prev_epoch_seal = (E, T+1)`, so epochs chain by exact ids across burned epochs. Cost: one seal per
touched namespace per epoch transition (cold path). The synthetic `{epoch−1, UINT64_MAX}` snapshot,
`sealed_from`, and the retroactive-void concept are deleted.

**INV-3 — the namespace universe is the catalog.** `cas/ref_catalog`, token-CAS like `gc/state`:
`namespace → {Creating | Live | Removing}` (round-5 major 6 adds `Creating` and the reconciliation
rules of §3). Written at DDL rate; read once per fold round; absent or undecodable on an initialized
pool = corruption, fail closed, never rebuilt from LIST. Capacity is charged at CREATION for the
entry's worst-case lifetime (its `Removing` form plus its fold-seal cursor), so a removal can never be
refused for capacity; namespace name length gains an explicit bound.

**INV-4 — the stream head is point-readable: `_ckpt`.** A stream whose prefix is CLEANED needs an
external pointer — with logs `1..100` deleted under a snapshot, `GET (E, 1)` returning 404 cannot
distinguish "cleaned" from "empty", so recovery cannot start from arithmetic alone and a hint-hidden
snapshot would mean state loss or a bricked empty namespace (round-5 blocker 1; this is forced, not
convenience). Each namespace gets `<ns>/_ckpt`, token-CAS, ~100 bytes:

```text
{ life_epoch, checkpoint_snapshot_id | none, last_epoch_seal | none }
```

Updated in exactly two places, both cold: snapshot publication (PUT snapshot → CAS `_ckpt.checkpoint`,
monotone; a crash between leaves an unreferenced snapshot as deletable debris) and epoch sealing
(records the seal id). **The append hot path never touches it.** Recovery reads catalog → `_ckpt` →
exact-key snapshot → arithmetic tail; the cleanup planner covers logs against `_ckpt.checkpoint`
instead of `snapshots.back()` from a listing — a stale `_ckpt` can only under-clean, never
over-clean. Absent `_ckpt` for a `Live` namespace = corruption → hold, loudly.

**LIST is demoted everywhere** to a zero-trust hint: which namespaces look active, bulk id prefetch,
garbage-candidate nomination. A hint omission costs a round of delay, never data.

## 3. Lifecycles {#lifecycles}

**Namespace creation:** catalog CAS `Creating` → create `_ckpt` (fresh `life_epoch`, empty pointers) →
catalog CAS `Live` → first append legal. A `Creating` entry whose `_ckpt`/first append never happened
is reconciled by GC after a bound (entry deleted, loudly). `dropNamespace` of a never-born entry
deletes it directly (today's no-op contract preserved).

**Removal:** admission = catalog CAS `Live → Removing` (this is the admission bound), then the
terminal `remove_namespace` record appends. A `Removing` entry whose terminal record is absent is
reconciled by the writer on next touch or by GC (append the terminal record idempotently — it is a
normal lane operation); no actor-less state exists. GC folds the terminal record, runs physical
cleanup, deletes the catalog entry and `_ckpt` last.

**Rebirth mints a fresh writer epoch** (round-5 blocker 3, light variant): recreation allocates a
fresh `life_epoch` via the existing durable epoch allocator, so the reborn stream's keys can never
collide with an undeleted old-life object, and `CasGc`'s recreated-keys-have-greater-epoch premise is
restored under INV-1. Plan-phase audit obligation: enumerate every namespace-scoped key family in
`CasLayout` and verify each is epoch- or build-qualified; qualify the exceptions (verbatim files are
the known suspect) or fall back to an explicit incarnation field in catalog + keys (the blinded
consult's scheme, recorded as the fallback).

## 4. Recovery {#recovery}

1. Catalog: entry state + `life_epoch` check. 2. `_ckpt`: checkpoint + last seal. 3. Exact-key `GET`
of the checkpoint snapshot (no listing involved). 4. Tail by arithmetic: expected next = `last + 1`;
hint omissions are fetched by exact key; a 404 below a durable higher id of the same epoch is
impossible-by-INV-1 → vanish-restart, then fail closed. 5. CAS-walk + seal the dead epoch (INV-2).
6. CAS `_ckpt` (seal id). 7. Install; the first new-epoch append stamps `prev_epoch_seal`. Acked ⇒
durable ⇒ dense ⇒ found: no trust discussion remains.

## 5. GC fold {#fold}

Per round: one catalog `GET`; ONE hint enumeration (strict `groupRefKeys` classification, feeding
intake, cleanup planning and the defer decision — round-5 minor 10); then per namespace with hinted
candidates (quiet namespaces cost zero):

- **Advance by arithmetic:** expected next = `cursor + 1`; `GET` it (owed anyway); fold; repeat. Hint
  holes — including the observed `0x1430c`/`0x1430d` shape — are folded through without noticing.
- **Cross epochs only by consumption:** an `EpochSeal` folds as a no-op; `(E', 1)` is accepted only if
  its `prev_epoch_seal` names the consumed seal. A wholly-hidden intermediate epoch cannot be skipped.
- **Frontier:** expected-next absent → stop (density: nothing acked can be missing). Optional probe
  skip when the hint showed nothing.
- **Hold:** impossible shapes (404 below a same-epoch witness, unconsumed-seal crossing) →
  per-namespace hold, loud, `suppress_destructive` pool-wide while carried (stated honestly), backoff,
  operator surface. Whole-round abort only for an unparseable key.

Cursor entries are keyed by the CATALOG; unhinted namespaces carry verbatim; retirement only with
catalog-entry deletion. Probe B1: `logs_accounted == logs_applied` over the cut, `EpochSeal` counted
as an applied no-op (B2 `produced=false`). Probe A stays as a SAMPLED store-quality detector (its
second enumeration runs on sampled rounds only) and aborts nothing. Cleanup: covered logs are
contiguous computable ranges under `_ckpt.checkpoint` + cursor; crossed dead epochs delete as closed
ranges `(E, 1..T+1)`.

## 6. Orphan-manifest sweep {#sweep}

Grants stay in their build's epoch (`requireAlive` rejects pre-remount builds), but **removals cross
epochs** (`maybeSweepStalePrecommits` appends an old build's precommit removal in the successor epoch
— round-5 major 9). Rules:

- a manifest of an epoch-`E` build is deletable only when the cursor has consumed epoch `E`'s
  `EpochSeal` **and** no unconsumed record in the arithmetic tail above the cursor names it as a
  removal target — the normative S42 fix: a removal whose body was deleted early would fold as
  "never activated" and strand the folded `+1`;
- otherwise retain — delay, never damage;
- live-epoch orphan BODIES are the writer's duty via a durable cleanup queue retried to completion
  (round-5 major 5: today only stale precommit BINDINGS are swept; bodies would leak until the epoch
  seals). A build is not retired while its cleanup duty or an owner-grant outcome is uncertain — the
  same rule INV-1's wedge change enforces on the grant side.

This section lands together with the S42 stale-edge fix as one coherent change to the sweep.

## 7. REBUILD and fsck {#rebuild-fsck}

REBUILD takes its universe from catalog + `_ckpt` + arithmetic tails. **It condemns nothing**: the
manifest side of its zero-edge comparison is LIST-fed and a hidden live manifest plus a listed blob
would condemn acked data (round-5 blocker 4), so zero-edge condemnation is disabled — REBUILD rebuilds
cursors and edges, omissions can only leak, and reclaiming genuinely-orphaned debris is left to the
normal sweep under §6's proofs. The stronger repair (an authoritative build/manifest registry or a
maintenance fence) is named future work. fsck: universe from the catalog, streams by arithmetic
(`chain-broken` fatal in summary AND exit code), tails above `_ckpt.checkpoint` only, `unchecked`
reserved for the genuinely unproven; a healthy pool returns clean.

## 8. Performance interactions {#performance}

Measured base (investigation §7.2): 3.42 M serial round trips per 30-minute round at ~0.5 ms; 256
logs/s intake; `1 + edges_per_log` `GET`s per log plus a HEAD per edge; 39.6 % cross-transaction
manifest re-reads; `pending_deletes` 77.2 s serial.

- P1 parallel fetch is de-risked: prefetch keys are arithmetic, mispredictions impossible, strict
  apply order untouched.
- ONE strict enumeration per round (probe A's second walk runs sampled); the defer decision rides the
  hint.
- Range cleanup: covered-log deletion generates keys arithmetically; dead epochs delete as ranges;
  shares P3's bounded budget.
- fsck replays tails, not histories.
- Unchanged: P2's round-scoped manifest cache (the 39.6 % is orthogonal); the HEAD-per-edge veto; the
  chaos-free rig (#10) before sizing.
- Named future lever: snapshot-diff folding for deeply backlogged cursors (the transient `+1/−1`
  pairs — exactly the 39.6 % — cancel in a state diff); own design, BACKLOG.
- P1+P3 share one pool-level concurrency budget.

Costs: append path +0 requests (allocator simpler than today); recovery +1 conditional PUT per
touched namespace per epoch transition (+1 per adopted straggler) and one `_ckpt` CAS; snapshot
publication +1 `_ckpt` CAS (async path); fold +1 catalog `GET` per round, one optional frontier probe
per active namespace; DDL +2 CAS (catalog, `_ckpt`). Formats: `EpochSeal` op kind, `prev_epoch_seal`,
catalog, `_ckpt`; DELETED: pool-wide sequence, synthetic snapshot ids, `sealed_from`, snapshot
discovery via listings. Pre-release; no compat scaffolding.

## 9. What this deletes {#deletions}

From v1–v4 drafts: per-record `prev` links; seal intervals, `NeverBorn`, void/breach rules; the seal
pointer and birth authority; `recovery_generation`; `R*` and its lease linearization; the CASed
admission object (catalog subsumes it); lineage tombstones; sticky floors; retention pins; the
trust-boundary essay. From current code: the pool-wide `next_ref_sequence` and safe-gap reasoning; the
synthetic recovery-seal snapshot machinery; snapshot discovery via `snapshots.back()` in recovery AND
in the cleanup planner; the second full enumeration as a correctness dependency; the carry-forward
special case; `_cleanup`-marker physical-empty rebirth gating (catalog + fresh-epoch rebirth subsume
it, plan-verified).

## 10. Verification {#verification}

All tests RED first; `HoleyListBackend` plus a delayed-PUT fault backend; negative controls for every
round-5 counterexample:

- hint hides middles → folded through by arithmetic; 404 below a witness → hold, siblings advance;
- `DefiniteFailure` after an ambiguous attempt → lane stays wedged, id NOT reused; the delayed first
  attempt lands and is adopted, never aliased (round-5 blocker 2's interleaving, through the real
  controller);
- CAS-walk both directions through `slot-occupy`: adoption of a landed straggler; seal-first →
  delayed ghost conclusively rejected (the flipped `LatePredecessorPut`); adopted `EpochSeal`
  terminates the walk; clean-transition sealing (an epoch with acked records closed cleanly still
  gets its seal on next touch);
- hidden checkpoint snapshot AFTER covered logs were cleaned → recovery restores from `_ckpt`, no
  state loss, no false-empty (round-5 blocker 1's scenario); stale `_ckpt` under-cleans only;
- warm rebirth: fresh `life_epoch` → no key collision with an undeleted old-life object; stale
  verbatim-file alias test per the §3 layout audit;
- REBUILD with asymmetric manifest/blob omission → nothing condemned (blocker 4);
- current-epoch died-mid-build bodies → writer queue reclaims them; build not retired under
  uncertainty;
- removal-tail retention: a manifest named by an unconsumed cross-epoch removal survives the sweep
  until the cursor passes the removal (major 9 / S42);
- catalog: `Creating` reconciliation both ways; `Removing` without a terminal record reconciled;
  capacity charged at creation (removal never refused); absent catalog / absent `_ckpt` fail closed
  loudly;
- B1 identity with `EpochSeal` no-ops; probe A sampled yet firing on a synthetic holey store; fsck
  clean on a healthy pool and `chain-broken`-fatal on a broken one.

TLA+ (phase 0): extend `_sab_holeylist` with INV-1/INV-2/INV-4; prove the cursor never passes an
unfolded record under arbitrary hint omissions, the flipped `LatePredecessorPut`, id-reuse safety
under the every-attempt rule, and the sweep's removal-tail retention. Then consult round 6 (this
document with all five rounds' findings and the blinded consult in context), then the soak gate.

## 11. History {#history}

- **Contiguity is the project's own I7 resurrected** (2026-07-10 spec); it died as collateral of the
  RefSnapLog-era model (full-base rewrites), never refuted on its merits; the pool-wide counter was
  adopted with "gaps are harmless" as the whole rationale — written before LIST was known to lie.
- **The ghost was documented** as `Late Predecessor PUT` (rev.4) with closure deferred on the "no
  extra request per ordinary mutation" constraint — which INV-2 honors.
- **Rounds 1–4** (`gpt-5.6-sol` `xhigh`) rejected the certificate stack and discovered the facts that
  shaped the invariants (burned epochs; graduation paced by rounds; unconditional build retirement;
  scheduling-only timeouts; universe circularity). **The blinded consult** independently reproduced
  the diagnosis and proposed the head-CAS chain (§12). **Round 5** validated the v5 skeleton (the A1
  per-namespace-id audit passed; grant epoch-locality confirmed) and forced: `_ckpt` (prefix cleaning
  makes an external head pointer mathematically necessary), the every-attempt reuse rule, fresh-epoch
  rebirth, REBUILD condemn-nothing, seal-on-every-transition, the `slot-occupy` primitive, catalog
  lifecycle completion, removal-tail retention, and the writer cleanup queue — all folded into v6.

## 12. Alternatives {#alternatives}

| alternative | disposition |
|---|---|
| v1–v4 certificate stack | Rejected by the user as accretion; deleted. |
| Full head-CAS commit chain (blinded consult) | North star if the wedge is ever worth deleting; not taken: moves the commit point (+1 CAS per flush — rev.4's refused cost), rewrites the hardest module, and needs N head reads or LIST-hints for discovery anyway. v6 borrows its catalog, its checkpoint idea (as the off-hot-path `_ckpt`), and its incarnation scheme as the rebirth fallback. |
| Checkpoint inside the catalog | Rejected: catalog writes would scale with snapshot publications pool-wide through one CAS object. |
| Never cleaning covered logs | Rejected: unbounded storage; `_ckpt` is the alternative's price, and it is small. |
| RefSnapLog-style combined mutable state | Historical perf rejection (whole-state CAS loop, painful truncation). |
| Local acked-floor / `_tail` / Keeper floor; enforced-timing ghost containment; widened probe A witness | Rejected earlier (user ruling; round-3 refutation; permanent-block risk) — unchanged. |

## 13. Out of scope, named {#out-of-scope}

The mount-time LIST probe (#23); decommission proven-dead fencing; snapshot-diff folding; P1/P2/P3
and the rig (#10); the 56 leaked blobs; the `-1`-before-`+1` path; the RustFS store-side mechanism
(unknown; this design holds either way); the authoritative build/manifest registry for a condemning
REBUILD.
