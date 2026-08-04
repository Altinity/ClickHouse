---
description: 'A condensed record of the paths CAS explored and rejected, and the major design pivots that produced the current architecture.'
sidebar_label: 'Design history'
sidebar_position: 13
slug: /antalya/cas/architecture/design-history
title: 'CAS Architecture — Design History'
doc_type: 'reference'
---

# CAS architecture — design history {#design-history}

This page is a condensed record of the roads not taken. The full narrative — every counterexample,
every commit, every dead end — stays in git history and `docs/superpowers/cas/how-we-got-here.md`
as future long-form material; what follows is the compact version: what was tried, why it was
rejected, and the sequence of pivots that produced the architecture described elsewhere in this
section.

## Rejected paths {#rejected-paths}

| What it was | What killed it | One line |
|---|---|---|
| **Generation-in-the-key** (Epoch-Based Reclamation core; blob keys carried a generation, `blobs/<hash>/<gen>`) | Adversarial review found `O(files)` persistent Keeper writes per commit and colliding intent keys across writers building identical content; a stuck writer stalled reclamation pool-wide | Replaced by the incarnation-token design: identity moved into the object body and delete precision into the backend token, removing the generation from every key |
| **Merkle tree layer** (a `Tree` object kind, `trees/<hash>` prefix, `child_gen` carried inside a tree's own identity) | Went with the generation-in-the-key core it was built on: a reclaim at any child propagated a new generation up the entire tree chain, and the tree layer was itself an extra surface for the same class of bug | Removed entirely; trees became manifest-internal, and `Blob` is the sole durable object kind besides the manifest and the ref |
| **Integer in-degree refcount** (a mutable counter, incremented per reference, decremented per release) | The decide-to-reference-then-not-yet-durable window let the fold observe in-degree 0 for a still-live blob; a mutable counter also costs a `CAS` round-trip proportional to write volume | Replaced by a derived count: `GC` folds a multiset of `+`/`-` source-edge deltas, so losing or duplicating a record can only delay reclamation, never accelerate one |
| **Zero-copy replication as the dedup mechanism** | Not rejected on a counterexample — a deliberate positioning choice made in the branch's first week | `CAS` coexists with zero-copy; it is opt-in per disk and solves the same duplication problem a different way — content-addressed pool sharing instead of per-blob Keeper reference counting — so no existing deployment needs to migrate |
| **Per-incarnation body keys** (`blobs/xx/<hash>.<incarnation>`, an alternative to the in-body incarnation tag) | Its own sabotage (a resurrect reusing the condemned incarnation instead of minting a fresh one) reintroduced the shared-key race; the design was also, structurally, generation-in-the-key again | Rejected in favor of the in-body `incarnation_tag` plus exact-token body delete, which keeps the generation out of every object key |
| **Meta as the lifecycle linearizer** (a per-hash `.meta` object whose presence/absence *was* the authority for a blob's lifetime) | The shipped code's marker is explicitly a point-read hint, never consulted by reads and never the linearization point — keeping a model that proved the linearizer framing would have asserted a guarantee the code does not make | The meta stays advisory: the in-body incarnation tag and exact-token delete are the real authority, and an absent meta reads identically to `Clean` |
| **Raw immutable bodies with a three-state tombstone meta** | A resurrect displacing the body forced a terminal-tombstone handshake — a writer↔`GC` liveness coupling that a permanent negative control (`sab_resurrect_from_tombstone`) showed re-enabled data loss | Rejected in favor of keeping the settled one-key-per-hash design with an in-body incarnation tag |
| **A persistent, append-only namespace registry for `GC` discovery** | Never deregistered on drop, so it grew monotonically forever; its fence cost scaled with namespaces ever created, not namespaces live | Deleted entirely; discovery moved to listing the ref-log prefix directly, proven safe by a two-coordinate proof (a durable per-shard incarnation plus a pool-global round) that neither coordinate alone can provide |
| **A separate all-shard fence-and-recheck phase per `GC` round** | Both phases cost `O(pool size)` GET+CAS every round regardless of churn — roughly 2.4 million requests at 100k tables — and the recheck's re-reads were the original quadratic hot spot | Replaced by a causal ack-floor: one streaming three-cursor merge per round, with no separate fence or recheck phase, cutting the request count by roughly three orders of magnitude |
| **A pool-wide sparse ref-id allocator with a certificate stack bolted on to prove completeness** | The additive fix (prev-links, seal intervals, quarantine states, sticky floors) kept growing through four review rounds without closing the root cause: absence is undecidable in a sparse id space | The invariants were changed instead of patched: dense per-life ids derived from applied state, an in-band epoch seal, and a `_ckpt` head object carrying the exact acknowledged frontier |

## Turns at a glance {#turns-at-a-glance}

| Date | Turn |
|---|---|
| 2026-06-01 | Starting point: "content-addressed storage for `MergeTree`" thesis and a working proof of concept |
| 2026-06-03 | `CAS` positioned as coexisting with zero-copy replication, not replacing it |
| 2026-06-07 | Adversarial review kills the write-ahead-intent mechanism and the epoch-based-reclamation core outright |
| 2026-06-10 | Incarnation-token design replaces generation-in-the-key |
| 2026-06-11 | The incarnation-core model clears a 782-million-state hunt plus 8.3 billion deep random visits with zero violations |
| 2026-06-12 | RustFS chosen as the soak testbed; an alternative backend rejected for silently ignoring conditional writes |
| 2026-06-18 | The B140 dangle is caught live in `system.cas_log`; per-blob revocable protection hints are replaced by structural build-root reachability |
| 2026-06-24 – 26 | Format convergence: every optional encoding collapses to two — a binary hashed format for immutable content and protobuf for the mutable hot path |
| 2026-06-26 | Root-local full-tree manifests collapse a forest of small `GC` objects into one hot/cold split |
| 2026-07-01 | The namespace registry is deleted; discovery moves to a two-coordinate incarnation-and-round proof |
| 2026-07-02 | Fence-and-recheck `GC` rounds are replaced by the one-pass, causal ack-floor round |
| 2026-07-06 – 10 | Writer/`GC` simplification: promote-time revalidation of tokened dependencies is proved redundant, and the freshness meta becomes an explicit advisory point-read |
| 2026-07-11 | The retired list is folded into the ref snapshot run, dropping a third cursor from the round |
| 2026-07-13 | Mount-lease boundary exclusivity (rev.6) closes the cross-epoch grace-window hazard at the handover, not with a timeout |
| 2026-07-15 | All part files become content-addressed (the mutable file set drops to empty); the disk-transaction dispatch collapses to one precommit contract |
| 2026-07-17 | An acked-then-lost `INSERT` is traced to a removed durability guard and fixed generically, upstream-flagged |
| 2026-07-21 – 22 | A full model-corpus audit re-checks every proof against shipped code; superseded models are removed rather than kept as false comfort |
| 2026-07-23 – 25 | The wide `INSERT` path and `GC` round costs are both measured directly, replacing folk explanations with request-count arithmetic |
| 2026-07-26 | A live probe catches a `LIST` omitting two already-durable ref entries — the incident that later settles the trust model for listings |
| 2026-07-27 – 28 | A first certificate-stack fix for the `LIST` hazard is rejected in review; the invariants are changed instead — dense ids, an in-band seal, and a `_ckpt` recovery frontier |
| 2026-07-29 | The new invariants land with destruction deliberately suppressed until the remaining consolidation work lands |
| 2026-08-01 – 03 | Recovery stops reading listings entirely, replaced by recovery-from-authority; the `LIST`-trust model is written down as settled |

## The pattern underneath {#the-pattern}

A handful of reflexes recur across these pivots, and are worth naming because they still apply to
new design work on this branch:

- **Re-derive the invariant, don't patch the mechanism.** Every "both halves are independently
  necessary and jointly sufficient" fix — build-root and fail-closed commit, trim-gate and
  cursor-in-snapshot, incarnation and round self-floor — came from asking what property must hold,
  not from patching the specific failure observed.
- **Delay is acceptable, authorization is not.** A stale-but-honest observation can only ever
  postpone a decision; the design consistently rejects any mechanism that could *accelerate* a
  destructive action past its safety gates, on the theory that a delay is a latency cost and a
  wrongful authorization is data loss.
- **A model that no longer matches the code is worse than no model.** Every removal in the corpus
  audit was justified this way — an unfaithful "proof" is false comfort, not documentation debt.
