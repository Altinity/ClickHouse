---
description: 'Snapshot of the open task list as of 2026-07-27, dumped to a file because the task descriptions carry detail that lives only in the session task store'
sidebar_label: 'Open tasks snapshot (2026-07-27)'
sidebar_position: 102
slug: /superpowers/cas/open-tasks-2026-07-27
title: 'Open tasks — 2026-07-27 snapshot'
doc_type: 'guide'
---

# Open tasks — 2026-07-27 snapshot {#open-tasks}

Dumped to a file because the descriptions below lived only in the session task store and would not survive
a restart. `todo-20260726.md` is the broader debt list; this is the working task queue with the reasoning
attached to each item.

Seven open, seven closed. Closed ones are summarised at the end so their conclusions are not re-derived.

---

## Open {#open}

### Build the GC bottleneck reproduction rig {#t10}

Deliverable 2 of the GC bottleneck study; deliverable 1 (per-phase rows) shipped. Unblocked now that the
GET multiplier is attributed.

Constraints the existing data forced:

- **Must be runnable CHAOS-FREE.** Every number we have comes from chaos runs, where frozen and killed
  processes are mixed into the tails and work is not separable from stall.
- **Must sweep arrival rate against the measured service rate** (~256 logs/s) so the queue-crossing point
  is observed rather than argued.
- **Must fold windows large enough to test the redundancy caveat.** The 39.6% manifest re-read figure was
  measured on a 959-log residual pool; the PREDICTION that a larger window captures more add/remove pairs
  and pushes it higher is unverified. This is where it gets checked before any cache is sized.

### Leak follow-ups: probe A blind spots, `-1`-before-`+1`, reconcile the 56 {#t11}

**The original framing was wrong and the reducer needs no fix at all.** In-degree is a SET; a residual `+1`
survives only if its `-1` never folded, and there are exactly two ways. The reducer is CORRECT in both: a
set cannot cancel an element it never received, and materialising a negative edge instead of dropping an
unmatched remove is how a false deletion would be born.

Three separable pieces:

- **A. Close probe A's two documented blind spots** — a hole reproducing IDENTICALLY in both enumerations,
  and a namespace dropped WHOLESALE from one (no `ref_tables` entry, so the comparison loop never visits
  it). Widening the witness rule was already considered and REJECTED: it would fire on legitimately-cleaned
  namespaces and block the cursor permanently.
- **B. Investigate `-1` arriving before `+1`.** Unexamined; 22 occurrences in a 20-minute soak is not rare.
  `unmatched_remove_example` already returns one example per round.
- **C. Reconcile the 56 already-leaked blobs.** No incremental round can reclaim them; only a rebuild of
  the in-degree state. A one-off operator action, separate from any code fix.

### Act on the four instrumentation-review recommendations {#t14}

Recorded and deliberately unexecuted, pending a decision:

1. Remove probe B1 — self-declared blind to the suspected defect, i.e. a signal that looks like coverage
   and is not.
2. Replace probe B2's per-transaction apply ledger with a scalar conservation check. B2 threads a field
   through the hot fold row and fails closed with a throw that wedges GC until a rebuild.
3. Remove the fsck detail-class whitelist in `utils/ca-soak/soak/fsck.py` — a class the product emits and
   the tuple omits is dropped silently. It already hid `awaiting-gc` for months, then
   `snapshot-oracle-mismatch` and `corrupted-run`.
4. Sample per-phase GC rows on ordinary successful rounds; 18 rows/round is 4.0 rows/s/disk worst case.
   Keep full detail on slow or failed rounds.

Weigh each against `INTENT.md` rather than applying wholesale — two of four remedies in the LAST review
were wrong, one of which would have turned a leak into a permanent wedge.

### Clear the Part B test debt and the `eraseView` window {#t15}

- Gate 0 has NO test at all, though the gtest declares the handoff.
- Some of the Part B review's missing tests are still missing.
- `test_cas_replicated_relink` is GREEN FOR THE WRONG REASON: a byte fetch onto a content-addressed disk
  dedups, so a flat blob count proves nothing about whether a relink happened.
- The residual post-commit window: `eraseView` can still throw AFTER the durable commit.

The first three are test correctness; the fourth is a real product window. Together because the tests are
how the fourth gets proven.

### Make the fsck entry gate actually run on a real pool {#t21}

Task #13 fixed the LIE, not the problem. The 2026-07-26 soak skipped its GC-checkpoint entry gate:
`entry-gate fsck timed out (exceeded 180s)` on a pool of only **5.5 GB**, and an earlier reading showed
`reachable=0` after 160 s — the scan had not finished its first phase. The degradation to a logged skip is
now honest, but **the gate does not run**, exactly when the pool is large enough to matter.

Options, none chosen:

- Scale the budget with pool size rather than a flat wall-clock number.
- Give the entry gate `--partial` deliberately and treat the result as a LOWER BOUND. Sound HERE and only
  here: a partial scan finding `dangling > 0` still fails correctly, so only the proof-of-absence direction
  weakens and the gate can report `unchecked` for that. **This is the opposite of what was reverted for
  `wait_for_pool_consistent`, and the reason differs** — that one is a waiter that needs proof, this is a
  one-shot entry check.
- Make fsck cheaper; it is dominated by the same ~0.5 ms per request measured for GC.

Every skipped gate is a checkpoint that proved nothing while looking green.

### Round-scoped manifest body cache — the measured 40% lever {#t22}

Measured, not proposed: 39.6% of intake manifest fetches are re-reads. 7,565 edges over 4,573 distinct
manifests, counted by decoding all 959 ref-log transactions on the stand.

**Intra-transaction redundancy is exactly ZERO.** All of it is cross-transaction: 2,991 manifests carry
both an add and a remove edge within one fold window, so the body is read once when the ref is published
and again when it is dropped. `CasManifestGet == CasRefEmittedEdges` exactly, which proves no cache exists
anywhere today.

A ROUND-scoped cache is therefore the lever, worth ~600 s of the 1830 s round; each avoided edge saves TWO
round trips. An op- or transaction-scoped cache would save NOTHING. Needs a byte bound — manifests are not
small and the fold already holds per-round buffers.

Verify the redundancy on a LARGE fold window first (see #10). Do NOT touch the HEAD-before-GET pair;
standing veto.

### LIST-consistency probe before trusting a store {#t23}

A standing backlog GATE that this round's finding promoted from prudent to necessary. It was written
speculatively; we now have PROOF that a store we run on daily returns an incomplete listing of a prefix it
has already durably written.

Two pieces:

1. **The probe itself**, in `Cas::Probe`, run at mount: write a known key set, enumerate, and refuse to
   trust LIST-derived discovery on a store that cannot return it. The capability-probe pattern the pool
   already uses.
2. **The real-S3 question.** Everything measured is RustFS. Whether AWS S3 does this is UNKNOWN and matters
   for urgency — though not for whether the fix is needed.

Note `list_consistency_hammer.py` is NOT this probe and did not reproduce the defect in three runs / ~19M
keys, including CAS's real start-after pagination. A mount-time probe has a different job — cheap
capability gating, not defect hunting — and should not be modelled on the hammer.

---

## Closed this round, with their conclusions {#closed}

| task | conclusion |
|---|---|
| Count the 4x GET multiplier | `S3GetObject` = log-body GETs + manifest-edge GETs, exactly. "4.15 per log" is `1 + edges_per_log`, and edges/log climbs 1.54 → 3.73 with backlog. |
| Settle probe A's firings | Cause found; superseded by the reopened task below. |
| fsck able to report on a large pool | Four defects fixed, not the one on the card: `corrupted_runs` invisible AND non-fatal, the timeout budgets inverted, a fabricated consistency proof the fix would have introduced, and a stale `M-F debris` label at three output sites. Plus four pre-existing RED harness tests cleared. |
| Distinct manifests vs edge count | 39.6% re-reads; zero intra-transaction. See #22. |
| Hammer RustFS directly | THREE valid runs, ~19M keys, ZERO holes — add-only, deletion-behind-cursor, and CAS's real start-after pagination. A negative that redirected the whole investigation. |
| Reopen probe A | RESOLVED: incomplete enumeration, observed and measured. Two objects durable for 19 s were omitted while a third written 2.2 ms later was returned. Every alternative excluded by measurement. |
| GC anomalies get a real audit event | `gc_anomaly` + HEAD verdict at firing time + two ProfileEvents registered in `signals.py`. Found the live defect four minutes into the first soak that carried it. |
| Run S42 at scale | OOM safety: PASS on every signal. But it reproduced a REAL defect — the orphan-manifest sweep stranding folded `+1` edges — root-caused 6/6. Full scale does not fit this host. |
