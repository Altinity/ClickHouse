---
description: 'v5 design for the LIST-incompleteness release blocker: three invariant changes — per-namespace contiguous ref-log ids, in-band epoch-close records that CAS-occupy the next slot, and a point-readable namespace catalog — replace the v1–v4 certificate machinery entirely; LIST is demoted to a zero-trust hint everywhere.'
sidebar_label: 'CAS ref contiguous-chain'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: contiguous ref streams and the in-band epoch seal'
doc_type: 'reference'
---

# CAS: contiguous ref streams and the in-band epoch seal {#cas-ref-contiguous-chain}

**Date:** 2026-07-27. **Status:** v5 — full reformulation after the user rejected v4's accretion;
awaiting adversarial review round 5 and user review. **Branch:** `cas-gc-rebuild`.

Fixes BACKLOG `{#list-as-journal-dataloss-2026-07-25}` — observed, not modelled
(`reports/2026-07-26-list-incompleteness-investigation.md`, evidence in
`reports/2026-07-26-list-incompleteness-proof/`) — and closes the `Late Predecessor PUT` limitation
documented in the Phase-1 ref spec (rev.4). Realizes P4 of `cas/draft-fixes-20260726.md` within its
refutation condition: zero added requests on the append path and on the fold's per-record path.

## 1. Problem, and why v1–v4 grew a tumor {#problem}

GC discovers ref-log transactions by listing `cas/refs/`, folds what the listing returned, and seals a
per-namespace cursor above what it OBSERVED, not what EXISTS. A record the listing omitted is sealed
past forever: a missed `-1` is a permanent leak; a missed `+1` lets GC delete a blob a committed
manifest still references. Writer recovery, the orphan sweep, REBUILD and fsck consume the same
untrusted listings.

Four adversarial review rounds against the v1–v4 drafts (all REJECT, §13) each patched the previous
patch, accreting: a per-record `prev` link, recovery-seal interval semantics, a `NeverBorn` variant, a
fixed-key seal pointer, a birth-time authority, a pool-wide recovery generation, an `R*` owner bound in
the lease, a CASed admission object, lineage tombstones, sticky floors, persistent quarantine. The
design owner rejected the accretion outright. The root cause of it, converged on independently by this
author and by a blinded consult (§13.5): **absence is undecidable in a sparse id space**, so every hole
demanded one more certificate. `next_ref_sequence` is pool-wide — one counter shared by every namespace
of a mounted writer — so within one namespace, id gaps are the norm and "what is missing" is not
computable.

The fix is not another certificate. It is changing the invariants that made certificates necessary.

## 2. The three invariant changes {#invariants}

**INV-1 — per-namespace contiguous ids.** `ref_sequence` becomes per-namespace: the next id is
`greatest_applied.ref_sequence + 1` (the value `commitRefChunk` already computes as its trial preview),
and an id whose outcome is PROVEN not durable (`DefiniteFailure`, `NoAttemptSent` pre-attempt refusal)
is REUSED by the next flush — the key is provably unwritten. The wedge discipline is untouched: an
uncertain PUT still blocks the lane, so at most one id per namespace is ever in flight. Consequence:
within `(namespace, epoch)`, durable ids are **dense**: `1..T`, no gaps, ever. The pool-wide
`next_ref_sequence` atomic is deleted. "What is missing" becomes arithmetic; the missing key's exact
name is computable; and point reads — which told the truth even while LIST lied (`head_verdict =
present`) — are the only reads correctness relies on.

**INV-2 — an epoch is closed in-band, by occupying the next slot.** Recovery finds the dead epoch's
tail with a **CAS-walk** that never trusts a listing: starting from the greatest replayed id `(E, k)`,
it `putIfAbsent`s an `EpochSeal` record at `(E, k+1)`.

- **Lost the race** → the slot holds a durable record (the dead session's at-most-one in-flight PUT
  that landed, or an acked record the hint listing hid): adopt it, replay it, `k+1`, repeat. The loop
  is bounded: at most one in-flight record exists (the wedge contract), plus whatever acked records
  the hint omitted — each iteration makes progress on real data.
- **Won the race** → epoch `E` is closed at `k`, and the ghost of the `Late Predecessor PUT`
  limitation can NEVER land: its only possible slot is taken, its conditional create is conclusively
  rejected by the store's own write-once guarantee. No timing argument, no grace, no interval, no
  authority object — the store's conditional create, which the pool already requires and probes, is
  the fence.

The `EpochSeal` is an ordinary ref-log record (a new op kind), folded as a state no-op. The first
record of the NEXT epoch carries one small field, `prev_epoch_seal = (E, T+1)` — the seal id the
recovering writer just wrote or adopted — so epochs chain by exact ids across arbitrary burned epochs,
and a fold can never cross from epoch to epoch past records it has not consumed (§5). The synthetic
`{epoch−1, UINT64_MAX}` recovery-seal snapshot, `sealed_from`, and the entire retroactive-void concept
are deleted: nothing is ever voided, because nothing can land behind a closed frontier.

**INV-3 — the namespace universe is a point-readable catalog.** One pool-level control object,
`cas/ref_catalog`, token-CAS'd exactly like `gc/state`: a sorted map `namespace → {Live | Removing}`,
~50–100 bytes per entry, written only at DDL rate — entry added (CAS) before a namespace's first
append is legal; `Live → Removing` when removal is admitted (this IS the removal-admission bound: the
catalog's size cap refuses new namespaces and new removals loudly, before the budget is exceeded);
entry deleted by GC after the terminal removal folded and physical cleanup completed. Absent or
undecodable catalog on an initialized pool = corruption: fail closed, loudly, never rebuilt from LIST.
The catalog is read once per fold round (one small `GET`) and answers the one question contiguity
cannot: *which streams exist* — closing wholesale-drop cursor loss, fsck's universe, and REBUILD's
condemn-the-unlisted hazard with a single object whose write rate is `CREATE/DROP TABLE`.

**LIST is demoted everywhere.** It remains as a zero-trust HINT: which namespaces look active this
round, bulk id prefetch, garbage-candidate nomination. A hint's omission costs one round of delay,
never data — every advance, every deletion premise, every universe claim rests on arithmetic, point
reads, conditional writes, and the catalog.

## 3. Writer {#writer}

Unchanged except the allocator, which gets simpler: per-namespace `greatest_applied + 1` under the
existing lane (delete the shared atomic and the safe-gap comments); reuse on proven-not-durable
outcomes; a conclusive different-object rejection at our key remains the anomaly it is today
(single-writer contract violation → fence). Chunked flushes number chunks sequentially. The
`EpochSeal` op kind and the `prev_epoch_seal` field of each epoch's first record are the only format
additions; both are stamped from values recovery already holds. Namespace creation CASes its catalog
entry before the first append (fail-closed); `remove_namespace` admission CASes `Live → Removing`
first, then appends.

## 4. Recovery {#recovery}

1. Hint-LIST the namespace prefix (bulk discovery of snapshot + log ids; zero trust).
2. Replay: newest snapshot, then logs upward **by arithmetic** — expected next id is `last + 1`; an id
   the hint omitted is fetched by its exact key; a point-`GET` 404 below a durable higher id of the
   same epoch is impossible-by-INV-1 (store hides a durable object) → the existing vanish-restart
   loop, then fail closed.
3. Close the dead epoch with the INV-2 CAS-walk; publish an ordinary state snapshot as today (real
   ids only — the synthetic-id machinery is gone).
4. Install; append lanes open; the first append of the new epoch stamps `prev_epoch_seal`.

The acked region needs no trust discussion anymore: acked ⇒ durable ⇒ its id is below the CAS-walked
frontier and dense ⇒ recovery read it or fetched it by exact key. The unacked in-flight record is
either adopted (it landed first) or permanently fenced out (the seal landed first). The `Late
Predecessor PUT` TLA action flips from demonstrating the counterexample to proving its absence.

## 5. GC fold {#fold}

Per round: one catalog `GET`; one hint-LIST of `cas/refs/`; then per namespace with candidates (a
quiet namespace — nothing hinted above its cursor — costs zero):

- **Advance by arithmetic.** Expected next = `cursor + 1`. `GET` it (the hint told us the body is
  probably there; the `GET` was owed anyway — one per folded record, exactly as today). Found → fold →
  repeat. This subsumes repair: a hint hole changes nothing, the observed `0x1430c`/`0x1430d` omission
  is folded through without noticing.
- **Cross epochs only through consumption.** An `EpochSeal` at `(E, T+1)` folds as a no-op and switches
  the expectation to "first record of the epoch named by a later record's `prev_epoch_seal`" — the
  fold accepts `(E', 1)` only if its `prev_epoch_seal == (E, T+1)`. A fully-hidden intermediate epoch
  is impossible to skip: the next epoch's first record names its true predecessor seal by exact id,
  and every id in between is reachable by arithmetic point reads.
- **Frontier.** Expected-next `GET` returns absent → that is the frontier (nothing acked can be
  missing — density), stop this namespace for the round. Optionally skip the terminal probe when the
  hint showed nothing new: pure delay, zero cost.
- **Hold.** A point-`GET` 404 below a durable higher id of the same epoch, an `(E', 1)` whose
  `prev_epoch_seal` names an unconsumed seal, or any impossible shape → per-namespace hold
  (`classification = 4`, loud `gc_anomaly`, `CasGcChainHolds`), cursor stays, siblings proceed;
  `suppress_destructive` remains pool-wide while any hold is carried (an unknown `+1` may name any
  shared blob — stated honestly), with escalating backoff on the namespace's retries and an operator
  surface. Whole-round abort remains only for an unparseable key in `groupRefKeys`.

Cursor entries in the fold seal are keyed by the CATALOG, not the listing: entries for unhinted
namespaces are carried verbatim; retirement happens only when GC deletes the catalog entry after
removal cleanup. Wholesale-drop of a namespace from a listing is thereby pure delay, and the v2
carry-forward patch plus all lineage-tombstone machinery collapse into this one rule. Probe B1's
identity is `logs_accounted == logs_applied` over the sealed cut, with `EpochSeal` records counted as
applied no-ops; there are no voids to account for. Probe A (the two-enumeration comparison) is KEPT as
a store-quality detector but demoted from the correctness path: it can run sampled (every Nth round),
and its firings no longer abort anything — the chain does not depend on listings.

Cleanup: covered logs below the cursor form contiguous ranges of computable keys — deletion needs no
listing and parallelizes under the shared budget; a crossed dead epoch is one closed range
`(E, 1..T+1)`.

## 6. Orphan-manifest sweep {#sweep}

All owner grants of a build live in the build's epoch (builds die with their mount). Rule: **a
manifest of a build from epoch `E` is deletable only when the namespace's cursor has consumed epoch
`E`'s `EpochSeal`** — at that point every grant that could name it is folded, and "no owner" is a
statement about complete knowledge; otherwise retain (delay, never damage). Live-epoch orphans are the
writer's own job — `maybeSweepStalePrecommits` already runs on authoritative in-memory state and
trusts no listing; its coverage of died-mid-build cases is verified in the plan. The `R*` bound, the
lease field, and the `min_active` linearization protocol are deleted. This section lands together with
the S42 stale-edge fix (the sweep stranding folded `+1` edges) as one coherent change.

## 7. REBUILD and fsck {#rebuild-fsck}

REBUILD takes its universe from the catalog and each namespace's state from snapshot + arithmetic
tail — no maintenance fence, no operator attestation, no lease-enumeration circularity. Its physical
blob LIST can only leak (an omitted blob stays), never condemn a hidden namespace's data. fsck gets a
provable universe (catalog), verifies streams by arithmetic (a `chain-broken` class that is a
`clean()` term and exit-code-fatal), replays only above the newest snapshot (its cost stops tracking
total ref-log volume — the 180 s budget problem shrinks structurally), and reports `unchecked` only
for what is genuinely unproven; a healthy pool can return clean.

## 8. Performance interactions {#performance}

Measured base (investigation §7.2): a 30-minute round is 3.42 M serial round trips at ~0.5 ms; intake
is 256 logs/s, request-bound, with `1 + edges_per_log` `GET`s per log and a HEAD per edge; 39.6 % of
manifest body reads are cross-transaction re-reads; `pending_deletes` hit 77.2 s serial.

- **P1 (parallel fetch, 4–16×) is de-risked and simplified:** prefetch keys are `cursor+1..cursor+K`
  by arithmetic — no dependency on listing contents, mispredictions impossible (404 = frontier),
  strict apply order untouched.
- **One enumeration instead of two:** the second full walk existed for probe A's correctness role;
  demoted to a sampled detector. The defer decision runs off the hint.
- **Range cleanup:** covered-log deletion generates keys arithmetically and shares P3's bounded
  concurrency; dead epochs delete as closed ranges.
- **fsck** replays tails, not histories, and reads its universe in one `GET`.
- **Unchanged and still wanted:** P2's round-scoped manifest body cache (the 39.6 % is
  cross-transaction and orthogonal to id shape); the HEAD-per-edge protocol step (standing veto); the
  chaos-free rig (#10) before sizing any of it.
- **Named future lever (not this spec):** snapshot-diff folding — for a deeply backlogged cursor,
  fold the NET owner diff between the snapshot at the cursor and the snapshot at the tip instead of
  every intermediate log; the transient `+1/−1` pairs (exactly the 39.6 %) cancel by construction.
  Needs its own design (precommit barriers, `mf_cleanup` from the dropped set); recorded in BACKLOG.
- P1+P3 share one pool-level concurrency budget setting.

Costs of this design: append path +0 requests (allocator simpler than today); recovery +1 conditional
PUT per namespace per unclean remount (+ one per adopted straggler); fold +1 catalog `GET` per round
and one optional frontier probe per ACTIVE namespace; DDL +1 catalog CAS. Formats: `EpochSeal` op
kind, `prev_epoch_seal` on epoch-first records, the catalog object; DELETED: pool-wide sequence,
synthetic snapshot ids, `sealed_from`. Pre-release, no compat scaffolding.

## 9. What this deletes {#deletions}

From v1–v4 drafts: the per-record `prev` link; recovery-seal intervals, `NeverBorn`, and every
void/breach rule; the seal pointer and birth authority; `recovery_generation` and graduation gating;
`R*`, its lease field and linearization; the CASed removal-admission object (the catalog subsumes it);
lineage tombstones and handoff; sticky recovery floors; seal retention pins; most of the trust-boundary
essay. From current code: the pool-wide `next_ref_sequence` and safe-gap reasoning; the synthetic
recovery-seal snapshot machinery; the second full enumeration as a correctness dependency; the
carry-forward patch as a special case; the `_cleanup`-marker-gated physical-empty rebirth checks
(subsumed by catalog states — verified in the plan).

## 10. Verification {#verification}

All tests proven RED before trusted; `HoleyListBackend` plus a new delayed-PUT fault backend:

- hint hides `0x1430c`/`0x1430d`-style middles → fold advances by arithmetic, folds them anyway;
- point-`GET` 404 below a same-epoch witness → hold, siblings advance, suppression carried;
- CAS-walk: seal loses to a landed straggler → adopted and replayed, then seals; seal wins → a delayed
  ghost PUT is conclusively rejected (the flipped `LatePredecessorPut`);
- an entire intermediate epoch hidden by the hint → `prev_epoch_seal` mismatch holds the namespace;
  burned epochs cross correctly;
- allocator: reuse after `DefiniteFailure`/`NoAttemptSent`; density under chunked flushes; wedge still
  blocks; saturating overflow;
- catalog: absent/undecodable → fail-closed loud; unhinted namespace's cursor carried; creation-before-
  first-append enforced; `Removing` admission bound refuses loudly at the cap;
- sweep: hidden newest promote → retained (epoch not sealed); sealed epoch → deletable; live epoch →
  writer-side sweep covers the died-mid-build case;
- fsck: `chain-broken` fatal both in summary and exit code; healthy pool returns clean; hidden
  namespace vs catalog → `unchecked`, never "empty";
- B1 identity with `EpochSeal` no-ops; probe A sampled yet still firing on a synthetic holey store.

TLA+ (phase 0): extend the `_sab_holeylist` model with INV-1/INV-2 and prove: the cursor never passes
an unfolded record under arbitrary hint omissions; the flipped `LatePredecessorPut` proves no record
lands behind a closed frontier; the sweep never deletes an owned manifest. Then adversarial consult
round 5 (this document, with all four prior rounds' findings and the blinded consult's alternative in
context), then the soak gate: repairs-by-arithmetic > 0 on a lying store, zero holds, zero aborts.

## 11. History and the archaeology that de-risks this {#history}

- **Contiguity is the project's own resurrected invariant.** The 2026-07-10 spec
  (`2026-07-10-cas-ref-snapshot-log-design.md`) had it as **I7 Contiguous replay** ("a gap before the
  maximum observed sequence is an exception"), one stream per incarnation, birth as sequence one. It
  died as COLLATERAL of abandoning the GC-owned-base model it lived in (full-base rewrites — the
  RefSnapLog performance dead end); the 2026-07-11 successor adopted the pool-wide counter with only
  "gaps are harmless" as rationale — written before anyone knew LIST lies. No recorded refutation of
  contiguity itself exists.
- **The ghost was a documented open limitation, not a discovery of this effort.** Rev.4 of the Phase-1
  spec names `Late Predecessor PUT` an "open correctness limitation", keeps a TLA action
  demonstrating it, and explicitly defers closure: "Phase 1 does not add a per-table `_seal`, mutable
  `_head`, or extra request to every ordinary mutation." INV-2 closes it while honoring exactly that
  constraint — zero extra requests on ordinary mutations; recovery pays one conditional PUT.
- **Review rounds 1–4** (`gpt-5.6-sol` `xhigh`, logs `tmp/codex_spec_review_sol*.log`): four REJECTs
  whose findings were all incorporated into v2–v4 and whose accumulated machinery this v5 deletes; the
  rounds' factual discoveries stand (burned epochs; graduation paced by rounds, not acks; unconditional
  build retirement vs wedged grants; `attempt_timeout` is scheduling-only) and shaped INV-1..3.
- **The blinded simplification consult** (`tmp/codex_simplify_probe.log`, prompt without any hint of
  this direction) independently reproduced the diagnosis ("absence is undecidable in a sparse ID
  space"), independently arrived at dense per-incarnation sequences, and proposed the alternative §12
  records.

## 12. Alternatives {#alternatives}

| alternative | disposition |
|---|---|
| v1–v4 certificate stack (prev links, seals-as-intervals, pointer, authority, generation, `R*`, tombstones) | Rejected by the user as accretion; every element deleted by INV-1..3. |
| **Full head-CAS commit chain** (blinded consult's pick: pool catalog + per-namespace mutable `_head`, commit point = head CAS, nodes inert until linked; wedge dissolves, fold needs no LIST at all) | Evaluated seriously; recorded as the north star if the wedge is ever worth deleting. Not taken now: it moves the COMMIT POINT (+1 head CAS per flush — the exact "extra request per ordinary mutation" rev.4 refused; ~+1–2 RTT per INSERT), rewrites the most battle-hardened module, and its pure form needs N head `GET`s per round for change discovery (regressing the one-LIST economy) unless it too re-adopts LIST-as-hint. The catalog is the one element borrowed. |
| RefSnapLog-style combined mutable state object | Historical: works, but the CAS loop serializes the whole growing state and truncation is painful; rejected on measured grounds in the project's past. |
| Local acked-floor / `_tail` / Keeper floor | User ruling stands; INV-2 makes them moot (the tail is CAS-walked, not declared). |
| Enforced-timing containment of the ghost | Refuted in round 3 (no enforceable relation; socket wait unbounded); INV-2 needs no timing. |
| Widening probe A's witness rule | Pre-existing rejection (permanent-block risk) — unchanged. |

## 13. Out of scope, named {#out-of-scope}

- The mount-time LIST-consistency probe (#23) — separate, tracked; still wanted as a store gate.
- The decommission/adoption proven-dead fencing (frozen-not-dead writer vs cross-node adoption) — the
  pool-member decommission spec; INV-2 narrows but does not own it.
- Snapshot-diff folding (§8) — future lever, own design.
- GC performance work P1/P2/P3 and the rig (#10); the 56 leaked blobs; the `-1`-before-`+1` path;
  the RustFS store-side mechanism (unknown; this design holds either way).
