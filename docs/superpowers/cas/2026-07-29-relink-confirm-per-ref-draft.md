---
description: 'DRAFT design context: per-ref refinement of the relink confirm''s rule 3 — restoring fetch-by-relink availability on busy writers without weakening fail-close'
sidebar_label: 'Relink confirm per-ref (draft)'
sidebar_position: 92
slug: /superpowers/cas/relink-confirm-per-ref-draft
title: 'Relink confirm: per-ref rule 3 (DRAFT for the design pass)'
doc_type: 'reference'
---

# Relink confirm: per-ref rule 3 — draft context for the design pass {#relink-confirm-per-ref-draft}

STATUS: DRAFT (2026-07-29). Not a spec. Input to a user-gated mini design pass; the protocol
rule it refines is spec-surface (`§confirm-primitive`), so nothing here is implementable
without that pass concluding. Companion BACKLOG item: `[RELINK-CONFIRM-BUSY-LANE]`
{#relink-confirm-busy-lane}.

## 1. Context: what the confirm is and why it exists {#context}

Fetch-by-relink (protocol v11, `260a6f81169`, 2026-07-25) lets a replica adopt another
replica's part by REFERENCE: instead of copying bytes, the receiver publishes its own ref to
the manifest the source offered. The blobs behind that manifest are protected only by the
SOURCE's committed binding, so v11's core safety step is: **the receiver confirms, after
preparing and before promoting, that the source still holds the exact binding it offered**
(`DataPartsExchange.cpp` T2-confirm before T3-promote). A `Yes` is an assertion about the
source's DURABLE table — it authorizes a remote promote.

The sender answers from `CasRefLedger::confirmExactRef` (`Pool/CasRefLedger.cpp:375`) under
two structural contracts, both load-bearing and both KEPT by this draft:

- **Zero object-store I/O.** The confirm runs on an interserver request; anything it could do
  is something a remote peer can make this writer do. It reads only resident state — never
  recovers, never resolves a wedge, never materializes a runtime (`find`, not `getRefTableRuntime`).
- **One snapshot across both lane mutexes** (`ref_queue_mutex` then `state_mutex`,
  try-to-lock). Admission happens under `ref_queue_mutex`, so an append is either entirely
  before the snapshot (visible as pending) or entirely after; there is no interleaving in
  which a removal is admitted and the function still answers `Yes`.

The current rules: 2 (residency+warm), **3 (lane quiescent)**, 4 (poison), 5 (exact row
equality — the only `No`), 6 (mount fence, last). Rule 3 verbatim:

> `if (rt.wedge.has_value() || !rt.pending.empty() || rt.leader_active) return Unknown;`
> "None of the three says anything about WHICH ref is affected, so all three are table-scoped
> refusals."

## 2. The measured problem {#measured-problem}

A busy writer's lane is essentially never quiescent (pending batches, active leader tenure),
so rule 3 refuses nearly always under sustained writes:

- T14 soak run 1: ~92k refusals vs 19,531 proven ≈ **17% availability**; sender at 168,955
  `CasRefBatchFlushes`.
- gc-audit (same run, independent count): **248,400 refusals in ~32 min** cluster-wide, peak
  **9,219/min**, each an ERROR-severity exception with ~23 symbolized frames — the refusal
  storm ≈ the entire `NETWORK_ERROR` population, and unwinding cost ≥ content hashing in the
  CPU top-frames.
- NOT a Stage-A regression: the stage's whole diff over `confirmExactRef` is a comment-only
  hunk (verified 2026-07-29).

Cost shape (verified at the throw site, `DataPartsExchange.cpp` row-3 taxonomy): abandon
throws `NETWORK_ERROR` = the retry-later class — the replication queue stores the fetch,
backs off, re-selects; a byte re-request to the same source is explicitly forbidden as
unsound. So refusals cost **convergence latency + noise**, never duplicate bytes and never a
double publish. Replication converged in both soaks (equal row counts). The criticality is
availability-of-the-mechanism: the flagship cheap-replication path is ~83% OFF exactly under
the load it exists for.

## 3. The key enabling fact: MutationScope already names the target {#mutation-scope}

`RefMutationItem.build_ops` is DEFERRED (ops materialize only at flush), so pending items'
op lists do not exist at admission. But `MutationScope` (`Pool/CasRefProtocol.h:39`) already
carries exactly the needed admission-time knowledge:

- `Kind::Ref` + `ref_name` — "touches exactly one ref name";
- `Kind::WholeShard` — dropNamespace and anything multi-ref, and the batch builder already
  flushes these SOLO and admits at most ONE mutation per ref name per flush.

So "which refs can the pending window touch" is knowable per item, at admission, with zero
new protocol state and zero I/O.

## 4. The refinement {#refinement}

Replace rule 3's three table-scoped terms with:

```text
refuse Unknown iff
    wedge.has_value()                                  -- unchanged, table-scoped (see §5)
 or busy_whole_shard != 0                              -- any WholeShard item in the window
 or busy_ref_names.contains(R)                         -- any windowed item scoped to R
```

where `busy_ref_names` (name -> count) and `busy_whole_shard` are maintained under
`ref_queue_mutex` with a lifetime of **admission → effects visible in `committed`** (or
pre-send refusal), deliberately INDEPENDENT of `pending` deque membership — this is what
covers the leader's in-flight chunk and the partially-durable window a chunked flush creates
mid-tenure. `leader_active` alone stops refusing: a tenure's non-row work (epoch seal,
snapshot publication, `_ckpt`) does not move committed rows, and every row-moving op it can
hold is in the index by construction.

Rules 2, 4, 5, 6 unchanged. Rule 5's exact `ManifestRef` equality plus mint-tightening
(§A3: repoint/recreate mints a fresh ref) keeps ABA closed. Rule 6's fence stays last.

## 5. Why fail-close is not weakened {#fail-close}

Every refusal that guarded an UNKNOWN-content window survives table-scoped:

- **wedge**: an unapplied, possibly-durable object whose contents are unknown without I/O —
  it may BE the removal being asked about. Stays a table refusal (rare; not the availability
  problem).
- **unrecovered / mid-recovery / superseded / poisoned / fence**: unchanged.

The ONLY relaxation: mutations whose targets are KNOWN BY NAME no longer block unrelated
refs. The original hazard sentence — "a pending item may be the removal being asked about" —
is now answered exactly (name match) instead of maximally (any item). A confirm for a ref
actually being repointed/removed right now still refuses; that is the genuinely-conflicting
case and the refusal is correct.

Sharp edges the design pass must nail down (none look fatal):

1. The index lifetime's far end: the precise point where an item's effects are stably in
   `committed` (leader apply vs `done=true` ordering under `state_mutex`) — decrement there,
   not at deque pop.
2. `WholeShard` inventory: enumerate every producer (dropNamespace, ...) and confirm each is
   either genuinely multi-ref or reclassifiable as `Ref`.
3. Failure paths: an item that errors after possibly-sending keeps its names busy until the
   wedge/resolution machinery settles it (ties into the wedge refusal).
4. Whether a tiny TLA model of the confirm-vs-window interleavings is warranted (recommended:
   safety property "never Yes while a windowed item targets R or WholeShard is present",
   including the chunked-flush partial-durability shape), or an exhaustive unit matrix
   suffices.

## 6. What does NOT change {#unchanged}

Wire format: nothing. Receiver behavior: nothing. Failure taxonomy: nothing. `ConfirmAnswer`
enum: nothing. The zero-I/O and one-snapshot contracts: kept verbatim. This is a sender-side
refinement of one rule plus one in-memory index.

## 7. Independent quick wins (non-protocol, can precede the pass) {#quick-wins}

- Demote the receiver's abandon from ERROR to INFO/DEBUG + a `ProfileEvents` pair
  (proven/refused) — the storm becomes a metric instead of log spam (measured: ~5-9k
  lines/min, ~23-frame unwind each).
- Sender-side: log WHICH rule refused (today `Unknown` maps silently through
  `ContentAddressedMetadataStorage.cpp:1996` — diagnosing rule 3 took a live cluster).

## 8. Expected outcome {#expected-outcome}

Relink availability on a busy writer goes from ~17% to ~100% minus true conflicts (same-ref
mutations in flight, WholeShard windows, wedges). The refusal storm and its exception cost
disappear with it. Convergence latency for fetched parts drops to the un-refused path.
