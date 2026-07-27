---
description: 'Design for closing the LIST-incompleteness release blocker: every ref-log record carries a prev link to its durable predecessor, the GC fold advances its cursor only along the verified chain (repairing listing holes with exact-key reads), recovery replay verifies the same chain, every destructive consumer of a listing-derived ref view is bounded by a chain-proven cut, and a stated trust boundary replaces any assumption that an enumeration is complete.'
sidebar_label: 'CAS ref-chain complete-cut'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: ref-log prev-chain and the complete-cut fold'
doc_type: 'reference'
---

# CAS: ref-log prev-chain and the complete-cut fold {#cas-ref-chain-complete-cut}

**Date:** 2026-07-27. **Status:** REVISED after adversarial review round 1 (verdict REJECT, all eleven
findings incorporated — §14); awaiting review round 2 and user review. **Branch:** `cas-gc-rebuild`.

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

The same untrusted-listing shape has more than one destructive consumer: the fold cursor (above), the
orphan-manifest sweep's protection set (§8), snapshot/log cleanup planning (§7), writer recovery
(§9), and fsck's verdicts (§10). This design covers all of them.

Why nothing simpler works: `next_ref_sequence` is pool-wide (`CasRefLedger.h`), shared by every
namespace of the mounted writer, so within one namespace arbitrary id gaps are the NORM. The candidate
ids of a suspected hole cannot be enumerated and probed — a missing id may belong to another namespace
or be a burnt safe gap — exactly as probe A's own comment states. Completeness therefore needs a
declaration from the writer, not a smarter reading of listings.

## 2. Trust boundary {#trust-boundary}

Stated once, load-bearing for every rule below (user ruling, 2026-07-27):

- **Hot-prefix LIST is UNTRUSTED.** The GC fold, the orphan sweep, and cleanup planning enumerate
  `cas/refs/` under concurrent appends and deletes. That is where the defect was observed. No
  destructive decision may assume such a listing is complete.
- **Cold-prefix LIST is TRUSTED.** Recovery (`ensureRefTableRecovered`) lists a namespace whose writer
  session has ended. The ACKED region is quiescent by construction: mount exclusivity fences the dead
  session, and its at-most-one in-flight log PUT is unacked and voidable. The permitted concurrent
  mutators of the prefix during recovery are enumerated, and each is handled, not assumed away:
  late-landing snapshot PUTs of the dead session; peer-GC log deletions (gated by durable cursor AND
  snapshot coverage — replay skips those logs anyway); peer-GC snapshot deletions (after §7's pin:
  only snapshots at or below the durable cursor); peer-GC `Removed`-snapshot publication for completed
  removals. Object-level races with any of these are absorbed by the existing bounded vanish-restart
  loop (fresh LIST, fresh selection), including the repair path (§9). A listing of previously written,
  quiescent content is trusted per the documented S3 strong read-after-write guarantee, which includes
  `LIST`.
- A store that violates the cold contract is a broken store: it is rejected by the mount-time
  LIST-consistency probe (BACKLOG `{#list-consistency-probe}`, task #23 — separate work), and if a
  violation ever leaves a trace anyway, the chain makes it LOUD (an off-chain record raises a
  `gc_anomaly` and holds the namespace) rather than silent.
- Point reads (`GET`/`HEAD` of an exact key) are trusted; conditional writes already depend on them, and
  at the observed firing the point read told the truth (`head_verdict = present`) while the listing lied.
- Consequence of the boundary: no local floor file, no `_tail` object, no Keeper floor. Rejected as
  needless state; the frontier of a cold listing is trusted, and only MIDDLE gaps are damage. For the
  fold, frontier omission is harmless delay — the cursor simply does not advance that far this round.
  For the SWEEP, frontier omission is NOT harmless, which is why its deletions are bounded by a proven
  cut (§8).

## 3. Invariant {#invariant}

**Complete cut.** A namespace's cursor advances only along the verified chain: every id between the old
and the new cursor is either (a) folded, in id order, or (b) certified void by a recovery seal.
Completeness is proved from the data; listing completeness is assumed nowhere. Destructive decisions
derived from a listing (sweep deletions, cleanup) are valid only at or below a chain-proven cut.

## 4. Writer changes {#writer-changes}

**The `prev` link.** Every ref-log transaction body gains one field:

- `prev : RefTxnId` — the id of the previous durable record of THIS namespace, across epoch boundaries.
  The value is `candidate_base_id = rt->state.getGreatestApplied()` in `commitRefChunk`. The wedge
  discipline keeps it exact — no id is minted while the previous outcome is unsettled — so the field
  encodes an invariant the lane already maintains. In a chunked flush every chunk links to the previous
  chunk, so the chain is continuous inside one tenure too.

**Seal adoption (review finding 1).** Publishing a recovery seal must also advance the private recovered
state's high-water to the seal id BEFORE `installRecoveryResult`, so the first post-recovery append
stamps `prev = {E, UINT64_MAX}` and the chain passes THROUGH the seal. Without this, the first new-epoch
record links to `sealed_from` directly, the fold accepts it by plain continuation, and the seal — the
only void certificate for the dead region — is never consulted.

**Empty dead region still seals (review finding 4).** Today a seal is published only when the recovery
listing observed at least one dead-epoch id. An UNCLEAN boundary over an EMPTY listing publishes
nothing, so a late-landing first record of the dead epoch (`prev = {0,0}`, chaining cleanly from a fresh
cursor) has no certificate and would be folded as if applied. Unclean recovery therefore ALWAYS
publishes a seal; `CasRefSnapshotFormat` gains an explicit empty-region encoding for `sealed_from`
(the current codec rejects a zero bound; the format is pre-release).

**Rebirth continues the chain (review finding 3).** `NamespaceBirth` does not reset the transaction
high-water (`RefTableState::applyOp`), so the first record of a recreated namespace links to the
`remove_namespace` transaction id of the previous lineage. The spec adopts that as the rule: ONE
monotone chain per namespace across removal and rebirth. `{0,0}` is the anchor for the FIRST life only.

Anchor forms of `prev`:

| form | meaning | acceptance rule at the consumer |
|---|---|---|
| `(E, s)` | ordinary link, including across a CLEAN epoch boundary and across removal/rebirth | `prev == resolved_through` |
| `{E, UINT64_MAX}` | anchored on the recovery seal of dead epoch `E` | seal object exists AND `resolved_through == seal.sealed_from` (the declared value verbatim — it may itself be a seal id when consecutive dead epochs chain; the empty-region encoding compares by its declared predecessor) |
| `{0, 0}` | first record of the namespace's FIRST life | `resolved_through == {0,0}` and no lineage tombstone exists for the namespace (§6) |

**Id hygiene (review finding 11).** `allocateRefTxnId` fails closed before `ref_sequence` reaches
`UINT64_MAX`, reserving the anchor sentinel from real allocation. The codec rejects any non-anchor
`prev >= txn_id`. Chain walkers carry cycle detection and a step bound.

Cost: ~16 bytes per record, zero added requests on the append path. Format: version bump of
`CasRefLogFormat` (and the `CasRefSnapshotFormat` empty-region encoding); decoders do not accept the old
versions — the pool format is pre-release and the no-compat-scaffolding rule applies.

## 5. Fold rules {#fold-rules}

The listing is only a CANDIDATE stream; the chain is the truth. **Rule precedence is explicit and
load-bearing (review finding 1): the void check runs FIRST.** A void record's `prev` chains cleanly by
construction (the dead writer's high-water was exact when it minted), so plain continuation must never
be reachable for a record the void rule covers.

For the next candidate X (ascending id, above `resolved_through`), after the body `GET` + decode the
fold already performs:

1. **Void.** X belongs to dead epoch `E`, a recovery seal for `E` exists (every unclean boundary now
   has one — §4), and `X.id > seal.sealed_from` → certify void: do NOT fold, record it in the round's
   accounting as `certified_void` (§5a). The writer retroactively erased this record ("born covered");
   folding it would diverge from writer state — this also closes the pre-existing hazard of a
   late-landing dead-epoch `-1` or `remove_namespace` being folded against a table that never applied
   it. Once the cursor crosses the epoch anchor, the void record sits below the cursor and
   `cleanupRefObjects` deletes it as ordinary covered debris.
2. **Continue.** `X.prev == resolved_through` → fold X, advance. Happy path: one comparison, zero added
   requests.
3. **Repair.** `X.prev > resolved_through` → the listing omitted links. Walk backward by exact-key `GET`
   (`refLogKey(ns, id)` is deterministic) until the chain reaches `resolved_through` or an anchor
   accepted by §4's rules, then fold forward in id order. **Bounded (review finding 10):** the walk
   retains bodies up to a byte budget; a longer run keeps only the ids, re-reads bodies forward in
   bounded chunks (doubling the `GET`s of the pathological run, never the happy path), and past a hard
   step bound holds the namespace instead. Loud: `CasGcChainRepairedHoles` + one `gc_anomaly` row per
   repaired hole; a repaired lie is still a lie and stays visible.
4. **Anchor.** `X.prev` is an anchor form → acceptance rules of §4's table. Lagging-cursor case (common
   under chaos — GC is behind a remount): if `resolved_through < seal.sealed_from`, the dead epoch
   still has unfolded records whose ids the pool-wide sequence makes non-enumerable — but `sealed_from`
   is itself a chain entry point with an exact key. Enter rule 3's walk at `sealed_from`, fold up to
   it, then accept the anchor.
5. **Hold.** A repair `GET` returns 404 (a chain-declared durable record invisible even to a point read
   — deposed-leader LOG cleanup cannot explain it: log deletion requires cursor AND snapshot coverage,
   both below `resolved_through`; seal deletion is excluded by §7's pin), or `X.prev <
   resolved_through` with no void certificate (chain split — impossible under the single-lane leader
   except as corruption or a cold-contract violation surfacing late) → **per-namespace hold**: keep
   `classification = 4` (the existing clamp semantics — re-read next round), cursor stays below the
   problem, loud `gc_anomaly`, `CasGcChainHolds`. Other namespaces of the round proceed. This is the
   "wait it out" branch, narrowed to one namespace.

The whole-round `ref_folding_aborted` remains only where no namespace can be attributed (an unparseable
key in `groupRefKeys`). Today's whole-round abort on "ref log body vanished mid-fold" becomes a
per-namespace hold under rule 5.

**Probe A is kept verbatim** — both walks, the comparison, the HEAD verdict at firing time, both
ProfileEvents, the 32-row cap (requirements of the investigation §8.2). Only the consequence changes:
instead of aborting the round, the chain either repairs (rule 3) or holds (rule 5). Its two blind spots
close structurally: an identical hole in both enumerations is repaired because discovery runs off `prev`
links, not off listing comparison; a wholesale-dropped namespace is covered by cursor lineage (§6) plus
no-advance, i.e. pure delay.

### 5a. Accounting: probes B1 and B2 over repaired and void records (review finding 6) {#fold-accounting}

One shared intake primitive processes listed AND repaired records, so a repaired record cannot bypass
the `TxnApplyLedger` (probe B2): every folded record — listed or repaired — gets an ordinal, a commit,
and an apply through the same path. Probe B1's identity becomes

`logs_accounted == logs_applied + logs_certified_void`,

where `logs_accounted` counts listed-in-range AND chain-repaired records. Negative controls in §11: a
dropped repaired delta and a listed-void certification must each turn the identity red.

## 6. Cursor lineage: carry-forward, no retirement (review findings 3, 5, 9) {#cursor-lineage}

Today the new fold seal's `per_ns_shard` is rebuilt only from the namespaces present in this round's
listing (both on the normal and on the abort path), so a namespace dropped WHOLESALE from one
enumeration loses its cursor entry: the next round either throws `CORRUPTED_DATA` at the baseline guard
(GC wedged until `SYSTEM CONTENT ADDRESSED GC REBUILD`) or re-folds from `{0,0}` and double-counts
in-degree (silent over-pin).

Fix: the new seal carries forward the parent entry of every namespace NOT visited this round, on both
paths — and **entries are never retired**. After a removal completes, the entry remains as the
namespace's **lineage tombstone**: the durable proof that ids up to the removal transaction were folded,
and the anchor against which a rebirth record (`prev = remove_txn_id`) is accepted even after cleanup
has deleted the removal log itself. Returning to `{0,0}` after retirement would accept a double-fold of
any log a cleanup listing had missed (review round 1, finding 3 scenario A) or hold forever on a deleted
`remove_txn` (scenario B).

Growth is bounded by the count of namespaces ever removed; each entry is ~a hundred bytes.
**Bounded-liveness statement (finding 9):** a removal whose physical cleanup never completes stays
`Pending` and is carried; when the pending set exceeds a configured bound the pool raises a loud event
(admission of new removals may then be refused — fail-loud, never silent growth). Compacting tombstones
into a denser durable object is named future work and must preserve the lineage proof.

## 7. Snapshot retention pin (review finding 5) {#snapshot-pin}

Log deletion is already gated by cursor AND snapshot coverage. Snapshot deletion is gated only by
"older than the newest" (`refCleanupPlan`), so a recovery seal — a chain ANCHOR — can be deleted while
the durable cursor is still below it; the next fold then 404s on `prev = {E, UINT64_MAX}` and the
namespace holds permanently. Fix, conservative: **a snapshot whose id is above the namespace's durable
fold cursor is not deletable.** Once the cursor crosses the anchor, the seal is ordinary history and
ages out as today.

## 8. Orphan-manifest sweep: deletions bounded by a proven cut (review finding 2) {#orphan-sweep}

`activeManifestKeys` reconstructs the namespace's owner set through `recoverRefTableDetailed` — a HOT
listing-trusting reconstruction — and the sweep deletes eligible manifests absent from that set. A
listing that omits a promote/commit log deletes a COMMITTED manifest: the same data-loss class as the
fold cursor, through a consumer the original spec draft missed. A backward chain cannot prove the
FRONTIER of a hot reconstruction (a listing returning only the snapshot offers no successor to walk
from), so repair alone does not fix the sweep.

Requirement: **the sweep may delete a manifest only when its non-membership in the owner set is proven
at a chain-verified cut that covers every transaction that could have granted that manifest an owner.**
Concretely: the reconstruction must be chain-verified (middle holes repaired, anchors honored) at least
up to the namespace's durable fold cursor, and a deletion is permitted only if the manifest's possible
owner-granting window lies at or below that proven cut; otherwise the manifest is RETAINED this round
(delay, not damage). The exact mechanism for bounding the owner-granting window (the relation between
the durable watermark floor that makes a build "provably dead" and the ids its owner transitions can
occupy) is a plan-phase task with its own consult — it interacts with the S42 stale-edge defect (the
sweep stranding folded `+1` edges), and the two should land as one coherent change to the sweep.

## 9. Recovery replay verification (review finding 7) {#recovery}

`ensureRefTableRecovered` gains the same chain check during tail replay: each replayed record's `prev`
must equal the previously replayed id, seeded by the selected snapshot's own id (a regular snapshot's id
is the greatest applied transaction at publish time, i.e. the last chain record it covers; anchor rules
of §4 apply at epoch boundaries and rebirth). A missing middle link → exact-key `GET` repair, same
primitive as fold rule 3 (`CasRefRecoveryChainRepairs`). A repair `GET` 404 is routed through the
EXISTING bounded vanish-restart loop — fresh LIST, fresh snapshot selection — because a legitimate race
(a late snapshot landing, then peer cleanup deleting a now-covered log) can 404 a link that a fresh cut
no longer needs. Only repeated absence across the restart budget fails recovery closed (the table stays
unrecovered and non-writable) with a loud event. The frontier above the listed maximum is trusted per
§2 (cold prefix); no tail declaration exists or is wanted.

## 10. fsck (review finding 8) {#fsck}

The fsck replayer performs the same chain walk. Verdict discipline:

- `chain-repaired` — informational; fsck point-read a link the listing omitted: read-only field
  evidence of store misbehavior.
- `chain-broken` — a declared link unreachable; counted in the SUMMARY and FATAL for the exit code (the
  `corrupted_runs` lesson: a `clean()` term must be visible twice over).
- **`unchecked` on an unproven frontier:** fsck cannot distinguish "the listing showed me everything"
  from "the newest logs or a whole namespace were omitted" (its view is hot, not cold). Where its
  verdict depends on frontier completeness it reports `unchecked` rather than clean — partial weakens
  proofs of absence, never evidence of presence. Both `chain-broken` and the unchecked state are terms
  of `clean()` and of the exit code. RED tests must hide the newest log and a whole namespace, not only
  a middle link.

## 11. Observability and verification {#verification}

New ProfileEvents: `CasGcChainRepairedHoles`, `CasGcChainHolds`, `CasGcChainVoidCertified`,
`CasRefRecoveryChainRepairs`. New per-phase metrics on the `fold_ref_intake` row: `chain_repairs`,
`chain_holds`, `certified_void`. All registered in the soak preflight (`utils/ca-soak/soak/signals.py`)
so a binary lacking them fails the run rather than reading zero. Audit: `gc_anomaly` rows for every
repair and hold, carrying the record id, `prev`, the expected link, the direction, and (for holds) the
HEAD verdict pattern probe A established. Counters die with the process; audit rows do not; ship both.

Verification, all tests proven RED before trusted:

- **Unit, on `HoleyListBackend`:**
  - hide a middle key from LIST with honest `GET` → fold repairs and folds in id order (the exact
    observed `0x1430c`/`0x1430d` scenario);
  - hide the key AND break the point read → the namespace holds, the cursor stays, sibling namespaces
    advance;
  - late-landing dead-epoch record above `sealed_from` → certified void, never folded — INCLUDING the
    empty-dead-region case (finding 4's `A={7,1}` scenario) and the precedence case (a void record
    whose `prev` equals the cursor must not be folded by plain continuation);
  - the first post-recovery log's decoded `prev` equals the recovery seal's id (finding 1);
  - seal-anchor acceptance, consecutive dead epochs, lagging-cursor entry via `sealed_from`;
  - rebirth: `prev = remove_txn_id` accepted against the lineage tombstone AFTER the removal log itself
    was cleaned (finding 3 scenario B); a `{0,0}` record rejected when a tombstone exists (scenario A);
  - wholesale-dropped namespace → seal still carries its cursor entry (both normal and abort paths);
  - snapshot pin: a seal above the durable cursor survives cleanup planning (finding 5 scenario);
  - sweep: hide the newest promote log from the sweep's listing → the committed manifest is RETAINED
    (finding 2 scenario);
  - B1/B2 negative controls: drop a repaired delta → identity red; certify a listed void → accounted;
  - id hygiene: sequence-overflow refusal, non-anchor `prev >= txn_id` rejected, repair-walk cycle
    bound.
- **TLA+ (phase 0 of the plan):** extend the `_sab_holeylist` model — which today proves the DEFECT
  mechanism sufficient — with the chain rule, void certification, and the sweep cut, and prove the fix
  sufficient: under arbitrary listing omissions the cursor never passes an unfolded non-void record,
  void certification never voids a record the writer applied, and the sweep never deletes an owned
  manifest.
- **Two independent strong-model consults**, prompted to refute each other. Round 1 (gpt-5.6-sol,
  xhigh): REJECT, eleven findings, incorporated here (§14). Round 2 reviews THIS revision, including
  whether any round-1 remedy is itself wrong.
- **Gate:** the existing soak; expected signature shifts from probe-A aborts to chain repairs
  (`CasGcChainRepairedHoles > 0` on a lying store, zero holds, zero aborts).

## 12. Performance {#performance}

P4's refutation condition was "if chain verification costs a request per record, it doubles intake".
It does not: the happy path adds zero requests and ~16 bytes per record — a `prev` comparison on a body
`GET` the loop already performs. Honest costs off the happy path (review finding 10): a repair costs the
body `GET`s the omitted records owed anyway, and a pathological over-budget run re-reads its bodies
forward in bounded chunks (doubling that run's `GET`s, bounding memory); a seal-anchor `GET` repeats
each round WHILE a clamp holds the cursor below the anchor — once crossed, never again. Strict
per-namespace apply order is unchanged, so the planned P1 fetch parallelization (order-preserving
prefetch) remains compatible — repair `GET`s are a rare serial path the prefetcher does not need to
know about.

## 13. Alternatives rejected {#alternatives}

| alternative | why rejected |
|---|---|
| GC-side containment only: per-namespace hold + scoped re-`LIST` + K-round stability quorum | Asks the same liar the same question; probabilistic, closes neither the identical-hole nor the enumerate-the-gap problem (pool-wide sequence). Its two sound elements — hold and point re-read — survive inside rules 3 and 5. |
| Local acked-floor file (pre-ack fsync), `_tail` object, or Keeper floor for the recovery tail | Rejected by the user 2026-07-27: needless state and fsyncs; the cold-prefix listing is trusted per the documented contract (§2), and the residual risk is made loud instead of being engineered around. |
| Widening probe A's witness rule to the pre-scan's own maximum | Previously considered and rejected: fires on a legitimately-cleaned namespace and blocks the cursor permanently. |
| `{0,0}` as the rebirth anchor with cursor retirement at `Completed` | Round-1 draft; rejected by review finding 3 — it destroys the only durable lineage proof and admits double-folds. Rebirth continues the chain; tombstones are permanent. |

## 14. Review round 1 {#review-round-1}

Adversarial review by `codex` `gpt-5.6-sol` at `xhigh` (2026-07-27, `tmp/codex_spec_review_sol.log`),
verdict **REJECT**. All findings verified against the code before adoption; all incorporated:

| # | severity | finding | landed in |
|---|---|---|---|
| 1 | blocker | seal not adopted as the writer's `prev`; void records reachable via plain continuation | §4 seal adoption; §5 precedence |
| 2 | blocker | orphan sweep deletes committed manifests from an incomplete hot LIST | §8 |
| 3 | blocker | `{0,0}` rebirth + cursor retirement destroy the lineage proof | §4 rebirth; §6 tombstones |
| 4 | blocker | empty unclean recovery publishes no seal → uncertifiable void first record | §4 empty seal |
| 5 | blocker | snapshot cleanup can delete a seal still needed as a chain anchor | §7 pin |
| 6 | major | probes B1/B2 undefined over repaired and void records | §5a |
| 7 | major | cold-prefix mutators understated; recovery repair-404 must use vanish-restart | §2; §9 |
| 8 | major | fsck can go green on an unproven hot frontier | §10 `unchecked` |
| 9 | major | `Pending` cleanup entries lack a bounded-liveness contract | §6 |
| 10 | major | repair buffering unbounded; seal-`GET` cost claim overstated | §5 rule 3; §12 |
| 11 | minor | `UINT64_MAX` sentinel unreserved; no `prev < id` validation; cycle risk | §4 id hygiene |

## 15. Out of scope, named {#out-of-scope}

- The mount-time LIST-consistency probe (task #23) — separate, already tracked.
- The soak-gating decision for probe A / chain counters (`todo-20260726.md` §0) — a policy decision.
- Reconciling the 56 already-leaked blobs (one-off operator action) and the `-1`-before-`+1` unmatched
  remove path (independent investigation, task #11B).
- GC performance work (P1 parallel fetch, P2 manifest cache, P3 parallel deletes) and the fsck budget.
- The store-side mechanism inside RustFS (unknown; this design holds either way).
