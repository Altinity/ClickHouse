---
description: 'Design for closing the LIST-incompleteness release blocker: every ref-log record carries a prev link to its durable predecessor, the GC fold advances its cursor only along the verified chain (repairing listing holes with exact-key reads), recovery replay verifies the same chain, the orphan-manifest sweep gains a provable owner-grant bound, REBUILD and fsck stop trusting hot listings, and a stated trust boundary with an explicit in-flight chronology replaces any assumption that an enumeration is complete.'
sidebar_label: 'CAS ref-chain complete-cut'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: ref-log prev-chain and the complete-cut fold'
doc_type: 'reference'
---

# CAS: ref-log prev-chain and the complete-cut fold {#cas-ref-chain-complete-cut}

**Date:** 2026-07-27. **Status:** v3 — revised after two adversarial review rounds (both REJECT; every
finding either incorporated or explicitly overruled with its argument recorded — §14/§15); awaiting
review round 3 and user review. **Branch:** `cas-gc-rebuild`.

Fixes BACKLOG `{#list-as-journal-dataloss-2026-07-25}` — observed, not modelled: an enumeration of
`cas/refs/` omitted two objects durable for nineteen seconds while returning a key written 2.2 ms after
them (`reports/2026-07-26-list-incompleteness-investigation.md`, evidence in
`reports/2026-07-26-list-incompleteness-proof/`). Realizes proposal P4 of `cas/draft-fixes-20260726.md`
("authoritative per-namespace chain plus a complete-cut gate") and satisfies its refutation condition:
the happy path adds zero requests.

## 1. Problem {#problem}

GC discovers ref-log transactions by listing the ref prefix, folds what the listing returned, and seals
the per-namespace cursor (`ShardCoverage::last_folded_ref_id`) above every record the round OBSERVED —
not above every record that EXISTS. The intake loop (`CasGc.cpp`, `fold`) iterates listed ids only, so an
id the listing omitted between two listed ids is sealed past silently and can never be folded again. A
skipped `-1` leaves a permanent retention leak; a skipped `+1` hides a live owner and lets GC delete a
blob a committed manifest still references — acknowledged-data loss.

The untrusted-listing shape has several destructive consumers, and this design covers all of them: the
fold cursor (§5), the orphan-manifest sweep's protection set (§8), snapshot/log cleanup planning (§7),
writer recovery (§9), `SYSTEM CONTENT ADDRESSED GC REBUILD` and fsck (§10).

Why nothing simpler works: `next_ref_sequence` is pool-wide (`CasRefLedger.h`), shared by every
namespace of the mounted writer, so within one namespace arbitrary id gaps are the NORM. The candidate
ids of a suspected hole cannot be enumerated and probed — a missing id may belong to another namespace
or be a burnt safe gap — exactly as probe A's own comment states. Completeness therefore needs a
declaration from the writer, not a smarter reading of listings.

## 2. Trust boundary and the in-flight chronology {#trust-boundary}

Stated once, load-bearing for every rule below (user rulings, 2026-07-27):

- **Hot-prefix LIST is UNTRUSTED.** The GC fold, the orphan sweep, cleanup planning, REBUILD and fsck
  enumerate `cas/refs/` under concurrent appends and deletes. That is where the defect was observed. No
  destructive decision may assume such a listing is complete.
- **Cold-prefix LIST is TRUSTED.** Recovery (`ensureRefTableRecovered`) lists a namespace whose writer
  session has ended. The ACKED region is quiescent by construction: mount exclusivity fences the dead
  session, and its at-most-one in-flight log PUT is unacked and voidable. The permitted concurrent
  mutators of the prefix during recovery are enumerated and handled, not assumed away: late-landing
  snapshot PUTs of the dead session; peer-GC log deletions (gated by durable cursor AND snapshot
  coverage — replay skips those logs anyway); peer-GC snapshot deletions (after §7's pin: only
  snapshots at or below the durable cursor); peer-GC `Removed`-snapshot publication. Object-level races
  with any of these are absorbed by the existing bounded vanish-restart loop, extended with a sticky
  floor (§9).
- **The in-flight chronology bounds the "ghost" window.** A record recovery did not see but a later
  fold listing shows would have to COMPLETE between the two. Under the documented store contract that
  requires one of exactly three exotic paths, because in every ordinary schedule an in-flight PUT
  settles before recovery's listing runs: on `kill -9` a partially-sent request dies with the
  connection and a fully-sent one commits within server-processing milliseconds, while recovery starts
  seconds-to-tens-of-seconds later (restart + lease expiry); on a freeze, kernel-buffered bytes commit
  DURING the freeze and user-buffered bytes commit at thaw+milliseconds, while the self-remount's
  recovery runs after thaw and later than that. The exotic remainder: (a) the store violating the cold
  contract; (b) a cross-node adoption racing a frozen-not-dead writer — the decommission path's
  proven-dead fencing problem, out of scope here; (c) a store committing a PUT with a long internal
  delay — no known mechanism. Consequence: the recovery-seal void machinery is DEFENSE-IN-DEPTH with
  loud detection (§5), not a workhorse; and no local floor file, `_tail` object, per-namespace anchor
  object, or Keeper floor is added — rejected as needless state (§13).
- A store that violates the cold contract is a broken store: it is rejected by the mount-time
  LIST-consistency probe (BACKLOG `{#list-consistency-probe}`, task #23 — separate work), and where a
  violation leaves a trace, the chain machinery makes it LOUD rather than silent.
- Point reads (`GET`/`HEAD` of an exact key) are trusted; conditional writes already depend on them,
  and at the observed firing the point read told the truth while the listing lied.

## 3. Invariant {#invariant}

**Complete cut.** A namespace's cursor advances only along the verified chain: every id between the old
and the new cursor is either (a) folded, in id order, or (b) inside a recovery seal's declared void
interval. Completeness is proved from the data; listing completeness is assumed nowhere. Destructive
decisions derived from a listing — sweep deletions (§8), cleanup (§7), REBUILD condemnation (§10) —
are valid only at or below a proven cut or under a proven-quiescent view.

## 4. Writer changes {#writer-changes}

**The `prev` link.** Every ref-log transaction body gains one field:

- `prev : RefTxnId` — the id of the previous durable record of THIS namespace, across epoch boundaries.
  The value is `candidate_base_id = rt->state.getGreatestApplied()` in `commitRefChunk`. The wedge
  discipline keeps it exact — no id is minted while the previous outcome is unsettled. In a chunked
  flush every chunk links to the previous chunk, so the chain is continuous inside one tenure too.

**Seal adoption (r1 finding 1).** Publishing a recovery seal also advances the private recovered
state's high-water to the seal id BEFORE `installRecoveryResult`, so the first post-recovery append
stamps `prev = seal_id` and the chain passes THROUGH the seal. The seal id is `{adopted_epoch − 1,
UINT64_MAX}` and mount retries can burn epochs, so the seal's epoch component does NOT name the dead
records' epoch — nothing below may key voidness by epoch (r2 finding 1); voidness is the INTERVAL
`seal.sealed_from < X.id AND X.id <= seal.snapshot_id`.

**Empty dead region still seals, as `NeverBorn` (r1 finding 4, r2 finding 3).** An unclean boundary
over an EMPTY recovery listing must still leave a durable certificate, and the current snapshot codec
cannot express it (`Removed` requires `remove_txn_id`; `Live` would corrupt a later `NamespaceBirth`).
`CasRefSnapshotFormat` gains an explicit never-born seal state carrying the void interval and no table
state. Unclean recovery therefore ALWAYS publishes a seal.

**Rebirth continues the chain (r1 finding 3).** `NamespaceBirth` does not reset the transaction
high-water (`RefTableState::applyOp`), so the first record of a recreated namespace links to the
`remove_namespace` transaction id of the previous lineage. ONE monotone chain per namespace across
removal and rebirth; `{0,0}` is the anchor for the FIRST life only.

Anchor forms of `prev`:

| form | meaning | acceptance rule at the consumer |
|---|---|---|
| `(E, s)` | ordinary link, including across a CLEAN epoch boundary and across removal/rebirth | `prev == resolved_through` |
| `{E, UINT64_MAX}` | the recovery seal published at the boundary the writer recovered across | seal object point-read at its exact key AND `resolved_through == seal.sealed_from` (verbatim; consecutive seals chain by their declared values; a `NeverBorn` seal declares its predecessor the same way) |
| `{0, 0}` | first record of the namespace's FIRST life | `resolved_through == {0,0}` and no lineage tombstone for the namespace exists in the fold seal (§6) |

**Id hygiene (r1 finding 11, r2 disposition).** `allocateRefTxnId` allocates with a saturating check
and fails closed before `ref_sequence` reaches `UINT64_MAX`, reserving the anchor sentinel and
excluding wrap-around of the shared counter. The codec rejects any non-anchor `prev >= txn_id`. Chain
walkers carry cycle detection and a step bound.

**Owner-grant bound `R*` (r2 finding 2, user decision).** The build sequence and the ref-transaction
sequence are independent counters with no recorded correspondence, which is what made the sweep's
"nobody references it" unprovable. The durable floor transition — the lease publication that advances
`min_active` past finished builds — now also records the member's ref-transaction high-water at that
moment, as a `RefTxnId` field beside `min_active`. Owner-granting transactions of a build settle before
the floor passes it (the floor advances only past terminal builds, and within a namespace's single lane
settled-before means smaller-id), so: **every owner grant of any build below `min_active` has id at or
below the recorded `R*`.** This is the one identifier-space link the sweep needs (§8).

Cost: ~16 bytes per ref-log record, one `RefTxnId` in the lease record, zero added requests on the
append path. Format: version bumps of `CasRefLogFormat`, `CasRefSnapshotFormat`, and the lease format;
decoders do not accept old versions — the pool format is pre-release and the no-compat-scaffolding rule
applies.

## 5. Fold rules {#fold-rules}

The listing is only a CANDIDATE stream; the chain is the truth. For the next candidate X (ascending id,
above `resolved_through`), after the body `GET` + decode the fold already performs:

1. **Continue.** `X.prev == resolved_through` → fold X, advance. Happy path: one comparison, zero added
   requests. This is also the live-epoch tail path and MUST stay probe-free: distinguishing a live
   epoch's next append from a ghost inside an unseen unclean boundary is not decidable from store data
   without a new per-namespace authority object, which was considered and rejected (§13); the ghost
   requires an exotic path per §2's chronology, and §5b detects it after the fact, loudly.
2. **Repair.** `X.prev > resolved_through` → the listing omitted links. Walk backward by exact-key
   `GET` (`refLogKey(ns, id)` is deterministic) until the chain reaches `resolved_through` or an anchor
   accepted by §4's rules, then fold forward in id order. **Bounded (r1 finding 10):** bodies are
   retained up to a byte budget; a longer run keeps ids only and re-reads bodies forward in bounded
   chunks (doubling that run's `GET`s, never the happy path's); past a hard step bound the namespace
   holds. Loud: `CasGcChainRepairedHoles` + `gc_anomaly` rows capped at 32 per round with the true
   total in every row (r2 finding 10).
3. **Anchor crossing.** `X.prev == {E', UINT64_MAX}` → point-read the seal at its exact key (the key is
   fully determined by the anchor value — no listing involved). Accept iff `resolved_through ==
   seal.sealed_from`; if `resolved_through < seal.sealed_from`, enter rule 2's walk at `sealed_from`
   (its exact key is declared by the seal) and fold up to it first. Listed records INSIDE the seal's
   interval `(sealed_from, seal_id]` are **certified void**: skipped, never folded (the writer
   retroactively erased them; folding a late `-1` or `remove_namespace` the table never applied is
   corruption), counted as `certified_void`, each with a LOUD `gc_anomaly` — under §2's chronology such
   a record should not exist, so its observation is evidence of an exotic path, never routine. Once the
   cursor crosses, void records sit below it and `cleanupRefObjects` removes them as covered debris.
4. **Hold.** A repair `GET` 404s (deposed-leader LOG cleanup cannot explain it — log deletion requires
   cursor AND snapshot coverage, both below `resolved_through`; seal and `Removed`-snapshot deletion is
   excluded by §7's pin), or `X.prev < resolved_through` without a covering seal interval (chain split),
   or an anchor's seal point-read 404s → **per-namespace hold**: `classification = 4` (existing clamp
   semantics — re-read next round), cursor stays below the problem, loud `gc_anomaly`,
   `CasGcChainHolds`. Sibling namespaces proceed. **Stated honestly (r2 finding 9): any hold keeps
   `suppress_destructive` set, and that flag is POOL-WIDE by design — an unknown `+1` may name any
   shared blob, so suppression must not be narrowed.** A held namespace enters a persistent quarantine
   with escalating backoff (its anchor/repair reads are not re-issued every round) and is surfaced for
   operator action (the `SYSTEM` control surface backlog item); the held-namespace age is a first-class
   metric.

The whole-round `ref_folding_aborted` remains only where no namespace can be attributed (an unparseable
key in `groupRefKeys`). Today's whole-round abort on "ref log body vanished mid-fold" becomes a
per-namespace hold under rule 4.

**Probe A is kept verbatim** — both walks, the comparison, the HEAD verdict at firing time, both
ProfileEvents, the 32-row cap. Only the consequence changes: the chain repairs (rule 2) or holds
(rule 4) instead of aborting the round. Its blind spots close structurally: an identical hole in both
enumerations is repaired off `prev` links; a wholesale-dropped namespace is pure delay via §6.

### 5a. Accounting: probes B1 and B2, cut-scoped (r2 finding 8) {#fold-accounting}

One shared intake primitive processes listed AND repaired records, so nothing bypasses the
`TxnApplyLedger` (probe B2), and B2 opens ordinals only for transactions whose deltas are committed
into the round. Probe B1 is **two-phase and cut-scoped**: the identity

`logs_accounted == logs_applied + logs_certified_void`

is evaluated over exactly the ids in `(parent_cursor, final_cursor]` — the sealed cut. Speculatively
repaired bodies and void observations ABOVE the final cut (a later clamp or hold pulled the cursor
back) are separate per-round metrics, not identity terms. Negative controls in §11.

### 5b. Post-hoc interval check at every crossing {#post-hoc-interval}

When an anchor is accepted, the fold also checks the ALREADY-SEALED side: if the parent cursor lies
strictly inside the seal's interval (`sealed_from < parent_cursor < seal_id`), then some past round
folded records the writer later voided — the exotic ghost path happened and prevention was impossible
(rule 1's decidability limit). This is detected HERE, loudly: a dedicated `gc_anomaly` outcome and
counter, quarantine of the namespace, operator escalation. Detection-after-the-fact is the deliberate
residual of the no-new-authority-object decision (§13), named per INTENT rather than softened.

## 6. Cursor lineage: carry-forward, tombstones, bounds (r1 findings 3/9, r2 finding 6) {#cursor-lineage}

The new fold seal carries forward the parent `per_ns_shard` entry of every namespace NOT visited this
round, on the normal AND the abort path — a wholesale-dropped namespace becomes pure delay instead of a
`CORRUPTED_DATA` wedge or a silent refold-from-zero.

After a removal completes, the entry is not dropped: it remains as the namespace's **lineage
tombstone** (`last_folded_ref_id == remove_txn_id`, ~50 bytes), the durable proof against which a
rebirth (`prev == remove_txn_id`) is accepted and a resurfaced old-life log is rejected, even after the
removal log itself is cleaned. The `Removed` snapshot is the point-readable backup of the same fact
(its key is derivable from the rebirth's `prev`), pinned while above the cursor (§7).

Bounds, stated as enforced rules rather than hopes (r2 disposition on finding 9):

- the cleanup backlog cap is enforced at ADMISSION — a new `remove_namespace` is refused, loudly, when
  the `Pending` set is at the bound — not reported after being exceeded;
- tombstones cost ~50 bytes in a control object with a 256 MiB ceiling; the count grows only with
  namespaces EVER REMOVED. A watermark metric and a loud pressure valve fire long before the ceiling;
  compaction of old tombstones into an archive object is named future work and must preserve the
  lineage proof.

## 7. Retention pins (r2 finding 7) {#snapshot-pin}

Log deletion is already gated by cursor AND snapshot coverage. Snapshot deletion is gated only by
"older than the newest", which can delete a chain ANCHOR before the cursor reaches it. Fix, narrow:
**recovery seals (ids of the syntactic anchor form) and `Removed` snapshots are not deletable while
above the namespace's durable fold cursor.** Ordinary snapshots keep today's newest-only retention —
pinning every snapshot above a held cursor would accumulate 64 MiB objects without bound (r2 refutation
of the v2 rule).

## 8. Orphan-manifest sweep: deletions bounded by `R*` (r2 finding 2, user decision) {#orphan-sweep}

`activeManifestKeys` reconstructs the namespace's owner set through a HOT listing and the sweep deletes
eligible manifests absent from that set — the same data-loss class as the fold cursor, through a
consumer the round-1 draft missed. A backward chain cannot prove the FRONTIER of a hot reconstruction,
so repair alone cannot fix the sweep; and no existing field relates build ids to ref-transaction ids.

With §4's `R*` recorded at every floor transition, the sweep's rule becomes provable:

- a manifest of build B is deletable only if **B is below `min_active`** (provably dead), **the
  namespace's chain-verified cut is at or above the `R*` recorded with that `min_active`** (so every
  transaction that could have granted B's manifests an owner is inside the verified cut), and **the cut
  shows no owner**;
- otherwise the manifest is RETAINED this round — delay, never damage. Retention is the only behavior
  on any uncertainty (missing `R*`, cut below `R*`, hold in effect).

The sweep's reconstruction itself switches to the chain-verified read path (same primitive as §9). This
section lands together with the S42 stale-edge fix (the sweep stranding folded `+1` edges) as one
coherent change to the sweep, since both alter its deletion premise.

## 9. Recovery replay verification (r1 finding 7, r2 finding 5) {#recovery}

`ensureRefTableRecovered` gains the same chain check during tail replay: each replayed record's `prev`
must equal the previously replayed id, seeded by the selected snapshot's own id; anchor rules of §4
apply at boundaries and rebirth. A missing middle link → exact-key `GET` repair
(`CasRefRecoveryChainRepairs`). A repair `GET` 404 is routed through the EXISTING bounded
vanish-restart loop (fresh LIST, fresh snapshot selection) — a legitimate race can 404 a link a fresh
cut no longer needs — **with a sticky floor (r2 finding 5): once a successor declared predecessor `P`,
every subsequent attempt must select a snapshot covering at least `P` or find `P`; a listing that
merely omits the successor does not lower the requirement.** The floor persists in the runtime across
touches; exhausting the restart brake fails recovery closed, loudly, with backoff — never a silently
smaller state.

## 10. REBUILD and fsck (r2 finding 4) {#rebuild-fsck}

`SYSTEM CONTENT ADDRESSED GC REBUILD` discovers its universe from one hot LIST and CONDEMNS every
physical blob absent from the rebuilt edge set — a wholesale-omitted live namespace would lose its
blobs. REBUILD therefore gains a precondition: a pool-wide quiescence proof (every member's mount lease
expired or fenced, or the pool held read-only — the `SYSTEM` control surface backlog item), which turns
its listings cold and trusted per §2. Without the proof it refuses to run.

fsck cannot prove its own universe either (`listNamespaces` is LIST-derived), and a per-namespace flag
cannot represent a namespace it never saw. Without a quiescence attestation the fsck run's
absence-based verdicts (reachability, orphan counts) are **whole-run `unchecked`** — positive findings
remain valid (partial weakens proofs of absence, never evidence of presence). The snapshot oracle's
silent skip when a seal or covered log is not listed becomes part of `unchecked`, never "checked". Both
`chain-broken` and the unchecked state are terms of `clean()` AND the exit code, with RED tests that
hide the newest log, a whole namespace, and a middle link — and one that proves `unchecked` actually
fires (a blanket flag that cannot go green would be its own green-that-cannot-go-red).

## 11. Observability and verification {#verification}

New ProfileEvents: `CasGcChainRepairedHoles`, `CasGcChainHolds`, `CasGcChainVoidCertified`,
`CasGcChainIntervalBreach` (§5b), `CasRefRecoveryChainRepairs`. New per-phase metrics on
`fold_ref_intake`: `chain_repairs`, `chain_holds`, `certified_void`, plus a held-namespace age gauge.
All registered in the soak preflight (`utils/ca-soak/soak/signals.py`). Audit: `gc_anomaly` rows for
every repair, hold, void certification and interval breach, capped at 32/round with true totals in
every row. Counters die with the process; audit rows do not; ship both.

Verification, all tests proven RED before trusted:

- **Unit, on `HoleyListBackend`:**
  - hidden middle key with honest `GET` → repair, fold in id order (the observed `0x1430c`/`0x1430d`
    scenario); hidden key AND broken point read → hold, siblings advance;
  - interval void: a record inside `(sealed_from, seal_id]` is skipped-with-anomaly even when its
    `prev` equals the cursor (the ghost shape), and §5b fires when the parent cursor already sits
    inside a crossed interval;
  - burned-epoch seal: records of epoch `E` covered by a seal whose id names a later burned epoch —
    interval semantics must certify them; epoch-keyed logic must not exist;
  - `NeverBorn` seal: published on empty unclean recovery; a late first record of the dead epoch is
    certified void against it; a later real `NamespaceBirth` proceeds;
  - the first post-recovery log's decoded `prev` equals the seal id (r1 finding 1);
  - rebirth: `prev == remove_txn_id` accepted against the tombstone after the removal log is cleaned;
    a `{0,0}` record rejected while a tombstone exists; a resurfaced old-life log rejected;
  - wholesale-dropped namespace → cursor entry carried on both normal and abort paths;
  - pins: a seal and a `Removed` snapshot above the durable cursor survive cleanup planning; an
    ordinary snapshot above a held cursor does NOT accumulate (newest-only retention preserved);
  - sweep: hide the newest promote log from the sweep's listing → the committed manifest is RETAINED;
    cut below `R*` → retained; cut at/above `R*` with no owner → deleted; missing `R*` → retained;
  - recovery sticky floor: a second listing omitting the successor must not lower the requirement
    (r2 finding 5's scenario);
  - B1 two-phase: identity over the sealed cut only; a dropped repaired delta and a certified listed
    void each turn it red; a hold above the cut does not;
  - id hygiene: saturating allocation, non-anchor `prev >= txn_id` rejected, repair-walk cycle bound.
- **TLA+ (phase 0 of the plan):** extend the `_sab_holeylist` model with the chain rule, interval void
  certification, and the sweep's `R*` bound; prove: under arbitrary listing omissions the cursor never
  passes an unfolded non-void record, void certification never voids a record the writer applied, and
  the sweep never deletes an owned manifest.
- **Independent strong-model consults, adversarial, iterated:** round 1 (gpt-5.6-sol, xhigh) REJECT —
  eleven findings; round 2 (same model, fresh context, tasked to refute round 1's remedies) REJECT —
  ten findings, eight round-1 remedies refuted in whole or part. All dispositions in §14/§15. Round 3
  reviews THIS revision, with the §13 overrules called out for attack.
- **Gate:** the existing soak; expected signature shifts from probe-A aborts to chain repairs
  (`CasGcChainRepairedHoles > 0` on a lying store, zero holds, zero interval breaches, zero aborts).

## 12. Performance {#performance}

P4's refutation condition was "if chain verification costs a request per record, it doubles intake". It
does not: the happy path — including the live-epoch tail — adds zero requests and ~16 bytes per record.
Honest costs off the happy path: a repair costs the body `GET`s the omitted records owed anyway; an
over-budget repair run re-reads its own bodies once more in bounded chunks; an anchor crossing costs
one seal point-read, repeated per round ONLY while a clamp holds the cursor below the anchor, and
quarantine backoff (§5's rule 4) stops even that for held namespaces; the sweep adds no requests beyond
its existing reconstruction (now chain-verified). Strict per-namespace apply order is unchanged, so the
planned P1 fetch parallelization remains compatible.

## 13. Alternatives rejected, including reviewer remedies overruled {#alternatives}

| alternative | why rejected |
|---|---|
| GC-side containment only: per-namespace hold + scoped re-`LIST` + K-round stability quorum | Asks the same liar the same question; probabilistic; closes neither the identical-hole nor the enumerate-the-gap problem. Its sound elements survive as rules 2 and 4. |
| Local acked-floor file, `_tail` object, or Keeper floor for the recovery tail | User ruling: needless state and fsyncs; the cold listing is trusted per the documented contract, §2's chronology bounds the residual, and violations are made loud. |
| Widening probe A's witness rule | Fires on a legitimately-cleaned namespace and blocks the cursor permanently (pre-existing decision). |
| `{0,0}` rebirth anchor + cursor retirement at `Completed` | Round-1 draft; destroyed the lineage proof (r2 finding 3). Rebirth continues the chain; tombstones are permanent. |
| Per-namespace fixed-key anchor/authority object (round-2 reviewer's remedy for finding 1; briefly proposed as v3 candidate) | **Overruled with the user.** Its only irreplaceable job is making the live-tail ghost decidable at fold time, but §2's chronology confines that ghost to exotic paths, §5b detects it after the fact loudly, and interval semantics plus the forced anchor crossing cover every decidable case. A new protocol object, written on recovery paths and read on fold paths, is not bought by an exotic-only residual. Recorded as the deliberate residual of this design. |
| Disabling orphan-manifest deletion for live namespaces (round-2 reviewer's smallest fix for finding 2) | Superseded by the `R*` bound (user decision): one small field at the floor transition makes the deletion provable instead of disabled. |
| Pinning every snapshot above the durable cursor (v2 §7) | Unbounded 64 MiB accumulation under a held cursor (r2 finding 7). Pin narrowed to seals and `Removed` snapshots. |

## 14. Review round 1 {#review-round-1}

`codex` `gpt-5.6-sol` `xhigh`, 2026-07-27, `tmp/codex_spec_review_sol.log`, verdict **REJECT**, eleven
findings — all verified against the code, all acted on; eight of its remedies were later refuted in
whole or part by round 2 and are superseded as recorded in §15. Mapping: seal adoption + void
precedence → §4/§5 (interval form); sweep → §8 (`R*` instead of deferral); rebirth/tombstones → §4/§6;
empty seal → §4 (`NeverBorn`); snapshot pin → §7 (narrowed); B1/B2 → §5a (cut-scoped); recovery
mutators/404 → §2/§9 (sticky floor added); fsck → §10 (whole-run `unchecked`); pending liveness → §6
(admission-time cap); bounded repair + honest costs → §5/§12; id hygiene → §4 (saturating).

## 15. Review round 2 {#review-round-2}

Same model, fresh context, tasked to refute round 1's remedies, `tmp/codex_spec_review_sol_r2.log`,
verdict **REJECT**, ten findings:

| # | severity | finding | disposition |
|---|---|---|---|
| 1 | blocker | void-first undecidable; seal epoch-keyed wrong under burned epochs | interval semantics adopted (§4/§5); the authority-object remedy overruled with the user — §13, residual detected by §5b |
| 2 | blocker | sweep remedy deferred the missing primitive | `R*` designed in (§4/§8), user decision |
| 3 | blocker | empty seal not encodable | `NeverBorn` state (§4) |
| 4 | blocker | REBUILD and fsck are hot-LIST destructive consumers | quiescence precondition; whole-run `unchecked` (§10) |
| 5 | major | vanish-restart can erase chain evidence | sticky floor (§9) |
| 6 | major | seal-carried tombstones unbounded; cap unenforced | admission-time cap, watermark valve, compaction named (§6) |
| 7 | major | pinning all snapshots unbounded | pin narrowed to seals + `Removed` (§7) |
| 8 | major | B1 undefined under holds/speculative work | two-phase cut-scoped identity (§5a) |
| 9 | major | per-namespace hold is a pool-wide reclamation wedge | stated honestly; quarantine/backoff + operator path (§5 rule 4) |
| 10 | minor | repair audit rows unbounded | probe-A-style 32-row cap (§5 rule 2) |

Its A–J cross-examination confirmed: seal high-water adoption is safe for existing consumers of
`getGreatestApplied`; no consumer semantically requires nonzero `sealed_from` (the codec was the only
rejection); the fsck oracle's seal skip must count as `unchecked` (§10).

## 16. Out of scope, named {#out-of-scope}

- The mount-time LIST-consistency probe (task #23) — separate, already tracked.
- The decommission/adoption path's proven-dead fencing against frozen-not-dead writers (§2 path (b)) —
  the pool-member decommission spec's problem.
- The soak-gating decision for probe A / chain counters — a policy decision.
- Reconciling the 56 already-leaked blobs; the `-1`-before-`+1` unmatched remove path (task #11B).
- GC performance work (P1/P2/P3) and the fsck budget.
- The store-side mechanism inside RustFS (unknown; this design holds either way).
