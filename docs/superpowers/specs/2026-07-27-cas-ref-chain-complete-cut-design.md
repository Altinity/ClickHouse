---
description: 'Design for closing the LIST-incompleteness release blocker: every ref-log record carries a prev link to its durable predecessor, the GC fold advances its cursor only along the verified chain (repairing listing holes with exact-key reads), recovery replay verifies the same chain, and a stated trust boundary replaces any assumption that an enumeration is complete.'
sidebar_label: 'CAS ref-chain complete-cut'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: ref-log prev-chain and the complete-cut fold'
doc_type: 'reference'
---

# CAS: ref-log prev-chain and the complete-cut fold {#cas-ref-chain-complete-cut}

**Date:** 2026-07-27. **Status:** DESIGN, approved in dialogue, awaiting spec review. **Branch:** `cas-gc-rebuild`.

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

Why nothing simpler works: `next_ref_sequence` is pool-wide (`CasRefLedger.h`), shared by every
namespace of the mounted writer, so within one namespace arbitrary id gaps are the NORM. The candidate
ids of a suspected hole cannot be enumerated and probed — a missing id may belong to another namespace
or be a burnt safe gap — exactly as probe A's own comment states. Completeness therefore needs a
declaration from the writer, not a smarter reading of listings.

## 2. Trust boundary {#trust-boundary}

Stated once, load-bearing for every rule below (user ruling, 2026-07-27):

- **Hot-prefix LIST is UNTRUSTED.** The GC fold enumerates `cas/refs/` under concurrent appends and
  deletes. That is where the defect was observed. No fold decision may assume such a listing is complete.
- **Cold-prefix LIST is TRUSTED.** Recovery (`ensureRefTableRecovered`) lists a namespace whose writer
  session has ended; the acked region is quiescent by construction (mount exclusivity; the at-most-one
  in-flight PUT of the dead session is unacked and voidable; concurrent peer-GC deletions touch only logs
  covered by both the durable cursor and a durable snapshot, which replay skips anyway; the
  vanish-restart loop covers the object-level race). A listing of previously written, quiesced content is
  trusted per the documented S3 strong read-after-write guarantee, which includes `LIST`.
- A store that violates the cold contract is a broken store: it is rejected by the mount-time
  LIST-consistency probe (BACKLOG `{#list-consistency-probe}`, task #23 — separate work), and if a
  violation ever leaves a trace anyway, the chain makes it LOUD (an off-chain record raises a
  `gc_anomaly` and holds the namespace) rather than silent.
- Point reads (`GET`/`HEAD` of an exact key) are trusted; conditional writes already depend on them, and
  at the observed firing the point read told the truth (`head_verdict = present`) while the listing lied.
- Consequence of the boundary: no local floor file, no `_tail` object, no Keeper floor. Rejected as
  needless state; the frontier of a cold listing is trusted, and only MIDDLE gaps are damage. For the
  fold, frontier omission is harmless delay — the cursor simply does not advance that far this round.

## 3. Invariant {#invariant}

**Complete cut.** A namespace's cursor advances only along the verified chain: every id between the old
and the new cursor is either (a) folded, in id order, or (b) certified void by a recovery seal.
Completeness is proved from the data; listing completeness is assumed nowhere.

## 4. Writer change: the `prev` link {#writer-prev}

The only writer change. Every ref-log transaction body gains one field:

- `prev : RefTxnId` — the id of the previous durable record of THIS namespace, across epoch boundaries.
  The value already exists at commit time: it is `candidate_base_id = rt->state.getGreatestApplied()` in
  `commitRefChunk`. The wedge discipline keeps it exact — no id is minted while the previous outcome is
  unsettled — so the field encodes an invariant the lane already maintains. In a chunked flush every
  chunk links to the previous chunk, so the chain is continuous inside one tenure too.

Three anchor forms of `prev`:

| form | meaning | acceptance rule at the consumer |
|---|---|---|
| `(E, s)` | ordinary link, including across a CLEAN epoch boundary | `prev == resolved_through` |
| `{E, UINT64_MAX}` | anchored on the recovery seal of dead epoch `E` | seal object exists AND `resolved_through == seal.sealed_from` (the declared value verbatim — it may itself be a seal id when consecutive dead epochs chain) |
| `{0, 0}` | first record of the namespace's life, or rebirth | `resolved_through == {0,0}`, or the last folded record of the old lineage is a folded `remove_namespace` transaction |

Cost: ~16 bytes per record, zero added requests, no new flush-protocol step. Format: version bump of
`CasRefLogFormat`; the decoder does not accept the old version — the pool format is pre-release and the
no-compat-scaffolding rule applies.

## 5. Fold rules {#fold-rules}

The listing is only a CANDIDATE stream; the chain is the truth. For the next candidate X (ascending id,
above `resolved_through`), after the body `GET` + decode the fold already performs:

1. **Continue.** `X.prev == resolved_through` → fold X, advance. Happy path: one comparison, zero added
   requests.
2. **Repair.** `X.prev > resolved_through` → the listing omitted links. Walk backward from `X.prev` by
   exact-key `GET` (`refLogKey(ns, id)` is deterministic) until the chain reaches `resolved_through` or
   an anchor accepted by §4's rules, then fold the collected records forward in id order, then X. Every
   repair `GET` is a `GET` the fold owed anyway — these are real records. Loud:
   `CasGcChainRepairedHoles` + one `gc_anomaly` row per repaired hole; a repaired lie is still a lie and
   stays visible.
3. **Void.** X belongs to dead epoch `E`, a recovery seal for `E` exists, and `X.id > seal.sealed_from` →
   skip WITHOUT folding. The writer retroactively erased this record ("born covered"); folding it would
   diverge from writer state — this closes the pre-existing hazard of a late-landing dead-epoch `-1` or
   `remove_namespace` being folded against a table that never applied it. Once the cursor crosses the
   epoch anchor, the void record sits below the cursor and `cleanupRefObjects` deletes it as ordinary
   covered debris. One seal-body `GET` per dead-epoch crossing per namespace, then never again.
4. **Anchor.** `X.prev` is an anchor form → acceptance rules of §4's table. Lagging-cursor case
   (common under chaos — GC is behind a remount): if `resolved_through < seal.sealed_from`, the dead
   epoch still has unfolded records whose ids the pool-wide sequence makes non-enumerable — but
   `sealed_from` is itself a chain entry point with an exact key. Enter rule 2's walk at `sealed_from`,
   fold up to it, then accept the anchor.
5. **Hold.** A repair `GET` returns 404 (a chain-declared durable record is invisible even to a point
   read — deposed-leader cleanup cannot explain it: its deletions are below the durable cursor and
   therefore below `resolved_through`), or `X.prev < resolved_through` with no void certificate (chain
   split — impossible under the single-lane leader except as corruption or a cold-contract violation
   surfacing late) → **per-namespace hold**: keep `classification = 4` (the existing clamp semantics —
   re-read next round), cursor stays below the problem, loud `gc_anomaly`, `CasGcChainHolds`. Other
   namespaces of the round proceed. This is the "wait it out" branch, narrowed to one namespace.

The whole-round `ref_folding_aborted` remains only where no namespace can be attributed (an unparseable
key in `groupRefKeys`). Today's whole-round abort on "ref log body vanished mid-fold" becomes a
per-namespace hold under rule 5.

**Probe A is kept verbatim** — both walks, the comparison, the HEAD verdict at firing time, both
ProfileEvents, the 32-row cap (requirements of the investigation §8.2). Only the consequence changes:
instead of aborting the round, the chain either repairs (rule 2) or holds (rule 5). Its two blind spots
close structurally: an identical hole in both enumerations is repaired because discovery runs off `prev`
links, not off listing comparison; a wholesale-dropped namespace is covered by cursor carry-forward
(§6) plus no-advance, i.e. pure delay.

**Probe B1 note:** `logs_intended` must count chain-discovered (repaired) records as intended, or the
`logs_intended == logs_applied` fail-closed identity raises a false exception on the first repair.

## 6. Cursor carry-forward {#carry-forward}

Today the new fold seal's `per_ns_shard` is rebuilt only from the namespaces present in this round's
listing (both on the normal and on the abort path), so a namespace dropped WHOLESALE from one
enumeration loses its cursor entry: the next round either throws `CORRUPTED_DATA` at the baseline guard
(GC wedged until `SYSTEM CONTENT ADDRESSED GC REBUILD`) or re-folds from `{0,0}` and double-counts
in-degree (silent over-pin). Fix: the new seal carries forward the parent entry of every namespace NOT
visited this round, on both paths. An entry is retired only deliberately: when the namespace's
`ns_cleanup_items` entry reaches `Completed` (removal physically confirmed empty). A dropped namespace
becomes pure delay.

## 7. Recovery replay verification {#recovery}

`ensureRefTableRecovered` gains the same chain check during tail replay: each replayed record's `prev`
must equal the previously replayed id, seeded by the selected snapshot's own id (a regular snapshot's id
is the greatest applied transaction at publish time, i.e. the last chain record it covers; anchor rules
of §4 apply at epoch boundaries and rebirth). A missing middle link → exact-key `GET`
repair, same primitive as fold rule 2 (`CasRefRecoveryChainRepairs`). A 404 on a chain-declared link →
recovery fails closed (the table stays unrecovered and non-writable) + a loud event. The frontier above
the listed maximum is trusted per §2 (cold prefix); no tail declaration exists or is wanted.

## 8. fsck {#fsck}

The fsck replayer performs the same chain walk. Two new detail classes:

- `chain-broken` — a declared link unreachable; counted in the SUMMARY and FATAL for the exit code (the
  `corrupted_runs` lesson: a `clean()` term must be visible twice over);
- `chain-repaired` — informational; fsck had to point-read a link the listing omitted, i.e. read-only
  field evidence of store misbehavior.

The soak-side fsck tuple must include both classes explicitly (the detail-class whitelist trap).

## 9. Observability {#observability}

New ProfileEvents: `CasGcChainRepairedHoles`, `CasGcChainHolds`, `CasRefRecoveryChainRepairs`. New
per-phase metrics on the `fold_ref_intake` row: `chain_repairs`, `chain_holds`. All registered in the
soak preflight (`utils/ca-soak/soak/signals.py`) so a binary lacking them fails the run rather than
reading zero. Audit: `gc_anomaly` rows for every repair and hold, carrying the record id, `prev`, the
expected link, the direction, and (for holds) the HEAD verdict pattern probe A established. Counters die
with the process; audit rows do not; ship both.

## 10. Verification {#verification}

- **Unit, RED first** (every test proven failing before trusted), on `HoleyListBackend`:
  - hide a middle key from LIST with honest `GET` → fold repairs and folds in id order (the exact
    observed `0x1430c`/`0x1430d` scenario);
  - hide the key AND break the point read → the namespace holds, the cursor stays, sibling namespaces
    advance;
  - late-landing dead-epoch record above `sealed_from` → skipped as void, never folded;
  - seal-anchor acceptance, consecutive dead epochs, rebirth after `remove_namespace`;
  - wholesale-dropped namespace → seal still carries its cursor entry (both normal and abort paths);
  - probe B1 identity holds across a repair.
- **TLA+ (phase 0 of the plan):** extend the `_sab_holeylist` model — which today proves the DEFECT
  mechanism sufficient — with the chain rule, and prove the fix sufficient: under arbitrary listing
  omissions the cursor never passes an unfolded non-void record, and void certification never voids a
  record the writer applied.
- **Two independent strong-model consults**, prompted to refute each other (standing rule for
  hard-concurrency changes).
- **Gate:** the existing soak; expected signature shifts from probe-A aborts to chain repairs
  (`CasGcChainRepairedHoles > 0` on a lying store, zero holds, zero aborts).

## 11. Performance {#performance}

P4's refutation condition was "if chain verification costs a request per record, it doubles intake".
It does not: the happy path adds zero requests and ~16 bytes per record; a repair costs exactly the
body `GET`s the omitted records owed anyway; a seal read is one `GET` per dead-epoch crossing. Strict
per-namespace apply order is unchanged, so the planned P1 fetch parallelization (order-preserving
prefetch) remains compatible — repair `GET`s are a rare serial path the prefetcher does not need to know
about.

## 12. Alternatives rejected {#alternatives}

| alternative | why rejected |
|---|---|
| GC-side containment only: per-namespace hold + scoped re-`LIST` + K-round stability quorum | Asks the same liar the same question; probabilistic, closes neither the identical-hole nor the enumerate-the-gap problem (pool-wide sequence). Its two sound elements — hold and point re-read — survive inside rules 2 and 5. |
| Local acked-floor file (pre-ack fsync), `_tail` object, or Keeper floor for the recovery tail | Rejected by the user 2026-07-27: needless state and fsyncs; the cold-prefix listing is trusted per the documented contract (§2), and the residual risk is made loud instead of being engineered around. |
| Widening probe A's witness rule to the pre-scan's own maximum | Previously considered and rejected: fires on a legitimately-cleaned namespace and blocks the cursor permanently. |

## 13. Out of scope, named {#out-of-scope}

- The mount-time LIST-consistency probe (task #23) — separate, already tracked.
- The soak-gating decision for probe A / chain counters (`todo-20260726.md` §0) — a policy decision.
- Reconciling the 56 already-leaked blobs (one-off operator action) and the `-1`-before-`+1` unmatched
  remove path (independent investigation, task #11B).
- GC performance work (P1 parallel fetch, P2 manifest cache, P3 parallel deletes) and the fsck budget.
- The store-side mechanism inside RustFS (unknown; this design holds either way).
