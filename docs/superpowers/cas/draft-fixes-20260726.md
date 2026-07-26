---
description: 'DRAFT fix proposals from the 2026-07-26 investigations — GC round duration and the LIST-incompleteness blocker. Each carries its motivating measurement, expected saving, risk, and what would refute it.'
sidebar_label: 'Draft fixes (2026-07-26)'
sidebar_position: 101
slug: /superpowers/cas/draft-fixes-2026-07-26
title: 'Draft fix proposals — 2026-07-26'
doc_type: 'guide'
---

# Draft fix proposals — 2026-07-26 {#draft-fixes}

**These are DRAFTS. Nothing here is implemented and nothing should be until the decisions in
`todo-20260726.md` §0 are made.**

Every proposal carries four things, and the fourth is the one that matters: **what would refute it**. A
proposal without a refutation condition is a wish, and this project has already spent a day on hypotheses
that felt obviously right.

Evidence base: `reports/2026-07-26-list-incompleteness-investigation.md`, BACKLOG
`{#gc-round-duration-answered}`, `{#gc-manifest-reuse-measured}`, `{#pending-deletes-shape}`,
`{#s42-ci-verdict}`.

---

## P1 — Parallel fetching in the GC fold {#p1-parallel-fetch}

**Motivating measurement.** A 30-minute round is **3.42 million serial round trips** at ~0.5 ms each. Phase
time is 100% accounted for by serial request latency across four large rounds (spread 1.15x); there is no
CPU term and no lock term. The loop is a plain `for` over tables with a synchronous GET per log and per
edge — no prefetch, no batching, no pool.

**Proposal.** Keep application strictly ordered; parallelise only FETCHING.

- Across tables: already independent by design — a clamp on one table does not stop others.
- Within a table: prefetch log bodies for ids above the cursor while the previous log is being applied.
- Manifest bodies: independent reads, fetchable ahead of use.

**Expected saving.** At N-way fetch concurrency, intake floor on the 404k-log round:

| N | floor | vs today's 1830 s |
|---|---|---|
| 4 | ~460 s | 4.0x |
| 8 | ~230 s | 8.0x |
| 16 | ~115 s | 15.9x |

**Risk.** Fold application order is a correctness property — logs of one table MUST fold in strict id
order, and a clamp must still stop that table at the right log. Any implementation that lets a prefetch
influence WHAT is applied, rather than only WHEN it is available, reintroduces the ordering bug class this
project has fought repeatedly. Memory: N in-flight manifest bodies must be bounded.

**What would refute it.** If a chaos-free measurement shows the phase is NOT request-bound — i.e. ms per
request stops being flat as concurrency rises — the model is wrong and the bottleneck is elsewhere (store
throughput limit, connection pool ceiling). **This is exactly what the rig (#10) should measure before any
implementation.** Note the two small rounds already sit at 1.2–1.3 ms/request instead of 0.5, unexplained;
if that is a per-round fixed cost rather than chaos noise, small rounds gain nothing from this.

---

## P2 — Round-scoped manifest body cache {#p2-manifest-cache}

**Motivating measurement.** **39.6% of intake manifest fetches are re-reads.** 7,565 edges over 4,573
distinct manifests, counted by decoding all 959 ref-log transactions on the stand.
**Intra-transaction redundancy is exactly ZERO** — all of it is cross-transaction: 2,991 manifests carry
both an add and a remove edge inside one fold window, so the body is fetched once when the ref is published
and again when it is dropped. `CasManifestGet == CasRefEmittedEdges` exactly, which proves there is no
cache anywhere today.

**Proposal.** A cache keyed by `ManifestId`, scoped to ONE fold round, bounded in bytes, dropped at round
end. Each avoided edge saves **two** round trips (HEAD + GET), not one.

**Expected saving.** On the 404k round: ~597k redundant edges x 2 trips x 0.5 ms ≈ **600 s of the 1830 s**.

**Risk.** Manifests are not small and the fold already holds per-round buffers; an unbounded cache trades a
latency problem for a memory problem, and S42 exists because this codebase takes allocation failure
seriously. The bound must be bytes, not entries. A round-scoped cache cannot serve stale data (the round is
a consistent cut by construction), so correctness risk is low — but only if it is genuinely dropped at
round end.

**What would refute it.** If the 39.6% does not hold on a LARGE fold window. It was measured on a 959-log
residual pool; the prediction that bigger windows capture MORE add/remove pairs is a prediction. If a large
round shows redundancy near zero — because add and drop land in different rounds at that scale — this
proposal saves nothing and should be dropped.

**Explicitly NOT part of this:** the HEAD-before-GET pair stays. It is a protocol step under a standing
veto. The cache avoids whole edges, which removes both trips together; that is not the same as removing
the HEAD.

---

## P3 — Parallel conditional deletes in `pending_deletes` {#p3-parallel-deletes}

**Motivating measurement.** 77.2 s in a single occurrence, against 243 ms for the next worst phase. The
phase is a nested serial loop issuing one `deleteExact` per object — at ~0.5 ms, roughly **150,000 deletes
one after another**.

**Proposal.** N concurrent conditional deletes. Each carries its own token, so exactness is preserved
per-object and the safety property is untouched.

**Expected saving.** Linear in N, same arithmetic as P1.

**Risk.** Low on correctness, real on store pressure: 150k concurrent-ish deletes against a store that is
already the bottleneck may worsen the timeouts seen in S42 ({#s42-ci-verdict}: 268-byte GETs timing out
under load). Concurrency must be bounded and probably shared with P1's budget rather than added to it.

**What would refute it.** If deletes are already rate-limited elsewhere for a reason nobody wrote down, or
if the store degrades non-linearly under concurrent conditional deletes.

**Ruled out, do not re-propose:** S3 bulk `DeleteObjects` (up to 1000 keys/call). `deleteExact` is
SAFETY-critical and token-conditional — it must remove ONLY the matching incarnation, and backends that
ignore the condition are rejected by `Cas::Probe`. Bulk delete carries no per-object precondition, so
batching would trade the guarantee that GC never removes a replaced incarnation for throughput. Wrong trade
at any speedup.

---

## P4 — The LIST-incompleteness blocker: proof of completeness, not a better listing {#p4-chain}

**Motivating measurement.** An enumeration returned ref log `0x1430e` and omitted `0x1430c` and `0x1430d`,
which had been durable for **nineteen seconds** and were written 2.2 ms BEFORE the one it returned. Every
alternative is excluded by measurement (`reports/2026-07-26-list-incompleteness-investigation.md` §2.3).

**Proposal (direction only — this needs a real design).** An authoritative per-namespace CHAIN plus a
complete-cut gate: a record whose predecessor link is unaccounted for is not a record the round may seal
past. Completeness becomes provable from the DATA rather than assumed from the listing.

**Expected effect.** Removes the class, rather than reducing its probability.

**Risk.** This is the largest change on the list and touches the cursor protocol, which is the part where
a mistake loses data rather than time. It should not be designed against the current measurements alone.

**What would refute the DIRECTION.** If chain verification costs a request per record, it doubles the
intake request count — the exact cost P1 and P2 are trying to remove. The design must carry its own cost
model, and if the cheapest correct chain is more expensive than the damage it prevents, the answer may be
containment instead (e.g. never advance the cursor past an unverified gap, accept the backlog).

**Constraints the evidence imposes:**

- Do NOT assume holes are page-shaped. They are short adjacent runs of 2–15 keys, not ~1000-key pages.
- Do NOT rely on store behaviour. RustFS demonstrably lies; AWS S3 is untested.
- KEEP probe A — it is why no damage occurred, and it is the only field detector for this class.
- KEEP the HEAD verdict at firing time — it separates two opposite defects and is unrecoverable afterwards.
- Close probe A's two blind spots via the chain, NOT by widening its witness rule (already considered and
  rejected: it would fire on legitimately-cleaned namespaces and block the cursor permanently).

---

## P5 — Small, independent, and worth doing regardless {#p5-small}

These need no decision and carry no design risk.

- **`--partial` on the fsck ENTRY gate** ({#fsck-budget-still-open}). Sound here and only here: a partial
  scan finding `dangling > 0` still fails correctly, so only the proof-of-absence direction weakens, and
  the gate can report `unchecked` for that. This is the OPPOSITE of what was reverted for
  `wait_for_pool_consistent`, and the reason differs — that one is a waiter that needs proof, this is a
  one-shot entry check. Today the gate simply does not run on a 5.5 GB pool.
- **Record the ref txn id on `ref_publish` / `ref_drop` audit events.** They carry none today, which is why
  the append-ordering question had to be answered through `blob_storage_log`. Would make writer-side
  ordering directly queryable.
- **A distinct-manifest counter per round.** Would turn P2's motivating number from a one-off decode into a
  continuously observable ratio, and would immediately answer P2's refutation condition on every round.

---

## Suggested order, and why {#order}

1. **P5's counter** — one small change that makes P2's go/no-go decidable from data instead of an argument.
2. **The rig (#10)** — a chaos-free baseline. Everything above rests on ~0.5 ms/request measured under
   chaos, where work and stall are not separable.
3. **P2**, if the counter confirms the redundancy holds at scale. It shrinks the request count.
4. **P1 and P3** together, sharing one concurrency budget. They divide what P2 left.
5. **P4** last, designed against numbers by then measured rather than assumed — and not before someone has
   decided whether the answer is a chain or containment.
