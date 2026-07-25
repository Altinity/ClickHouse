---
description: 'Design for four decided follow-ups on the content-addressed branch: a mechanism-agnostic detector for "the fold cursor advanced past a transaction that was never applied", per-phase rows in `system.content_addressed_garbage_collection_log`, a rework of the S42 scenario verdict so consistency assertions decide, and an opt-in mount-time force-claim that overrides a differing server uuid and mounts as WRITE.'
sidebar_label: 'CAS GC Observability And Mount Force'
sidebar_position: 20260725
slug: /superpowers/specs/cas-gc-observability-and-mount-force-design
title: 'CAS: skipped-transaction detector, per-phase GC rows, S42 verdict, mount force-claim'
doc_type: 'reference'
---

# CAS: skipped-transaction detector, per-phase GC rows, S42 verdict, mount force-claim {#cas-gc-observability-and-mount-force}

**Date:** 2026-07-25. **Status:** DESIGN, awaiting approval. **Branch:** `cas-gc-rebuild`.

**Settles:** the four decisions recorded in `docs/superpowers/cas/BACKLOG.md`
{#user-decisions-2026-07-25} plus {#list-as-journal-decision-c}. Executed as ONE batch AFTER the
Part B codex review and BEFORE the Part B soak.

**Plan:** `docs/superpowers/plans/2026-07-25-cas-followups-detector-introspection-s42-forceclaim.md`.

## Sizing, stated up front {#sizing}

Three of the four items are contained. One is not as small as the decision text makes it sound, and
one turned out smaller:

- **Item 1 (detector)** — probe A is essentially FREE (see {#probe-a}: the round already performs the
  two independent enumerations it needs, so the detector costs a set comparison and no extra I/O).
  Probe B is **not** free and is **not** small: making "intended-to-fold vs actually-applied"
  non-vacuous requires carrying a per-transaction ordinal through `BlobDelta` — the hot fold row —
  into the shard reducers. That is a change to the fold's inner data structure. It is still worth
  doing (the decision names exactly the loss class only probe B can see, and the same carry is fix
  item 3 of the RCA), but it should be reviewed as a hot-path change, not as instrumentation.
- **Item 2 (per-phase rows)** — the largest by diff size (17 phases, a schema change, a converter,
  docs) but the lowest risk: additive, no behaviour change, no format change.
- **Item 3 (S42)** — genuinely small. Four edits. But it contains one trap the decision text does not
  mention: fixing the soundness guard alone does **not** let S42 read green, because a second
  verdict — `Verdict.skipped` at `s42_alloc_faults.py:518` — also outranks `pass`. Both must move.
- **Item 4 (force-claim)** — small in code, but the naive shape ("force the owner uuid") does not
  work: after rewriting the owner, the *mount slot* still carries the predecessor's uuid and
  `claimMount` refuses it as `ForeignOwner`. The force has to cover both objects, and the mount-slot
  half is exactly where the liveness protection lives. See {#force-two-objects}.

---

## Item 1 — a mechanism-agnostic detector for a skipped transaction {#item-1-detector}

### What must be detected {#detect-what}

Not "a hole in a `LIST` page". The effect:

> the fold cursor advanced past a ref-log transaction that was never applied.

The `LIST` hypothesis is UNCONFIRMED (BACKLOG {#list-as-journal-decision-c}): a holey page was never
directly observed, it survives by elimination, and TLA+ (`CaRelinkConfirmCore.tla` `_sab_holeylist`)
proves the mechanism is SUFFICIENT, not that it is what happened. A detector built for that
hypothesis would stay silent if the record is lost lower down — delta routing across gc shards, the
reducer, the run flush — and would manufacture false confidence while doing so.

### Why "a gap in the id sequence" is not the signal {#gaps-are-legitimate}

Ref transaction ids are **not required to be contiguous**, and this is deliberate, not incidental:
Task 18 (`252ccbdf2d4`) leaves a safe gap whenever an append is refused before any attempt reaches
the network. A round that sees `…, 17, 19, …` has seen nothing anomalous.

The obvious repair — enumerate the missing ids in `(cursor_prev, resolved_through]` and `HEAD` each
one — does not work either. `RefTxnId` is `(writer_epoch, seq)` with `writer_epoch` primary; a single
writer restart advances the epoch and makes the interval between two consecutive observed ids
unbounded. There is nothing finite to enumerate. This rules out the whole "probe the gaps" family.

**What IS unambiguous** is existence-relative-to-a-witness, not absence:

> An id `x` for namespace `ns` that is **present in one enumeration** of `cas/refs/`, **absent from
> another**, and **strictly below the maximum id the other enumeration returned for that same
> namespace**, cannot be explained by a concurrent append.

The ref log per namespace is append-only with strictly increasing ids. If enumeration E2 returned
some id `> x` for `ns`, then `x` was already durable when E2 ran; E2 not returning `x` therefore means
either the store gave an incomplete answer or something between the store and us dropped it. No
legitimate concurrent writer can produce that shape. This is the entire basis of probe A, and it needs
no assumption about pagination, cursors, or page boundaries.

The dual — `x` present in E2, absent from E1, below E1's max — is equally unambiguous and catches the
hole in the other enumeration.

The one legitimate way to break it is a **deletion** between the two enumerations. Only GC's own
`cleanupRefObjects` deletes ref logs, and it runs post-CAS at the very end of a round, so it cannot
run between two enumerations of the same round on the same leader. A **deposed** leader still
executing its post-CAS cleanup can. That case is itself an anomaly worth suppressing the round for —
this round's cursor must not advance while another process deletes ref objects under it — so it is
reported through the same channel, with a note naming the alternative explanation.

### Probe A — "the store lied", at zero I/O cost {#probe-a}

The load-bearing observation: **a GC round already performs two full, physically separate
enumerations of `cas/refs/`**, and throws the first one away.

- `Gc::changedShardCount` (`Gc/CasGc.cpp:1910`) runs `forEachListedKey(backend, layout.casRefsPrefix(), …)`
  over the whole prefix (`:1923`), keeps only a per-namespace maximum log id, and discards the rest.
  It is called unconditionally on every round, at `CasGc.cpp:349`, before the defer decision.
- `Gc::fold` (`:846`) runs the same enumeration again into `ref_object_keys`.

These are two separate physical `LIST` walks against the same prefix, seconds apart at most, each
paginating independently. They are as close to an independent second derivation as the backend can
offer, and one of them is already paid for. Probe A is therefore a **set comparison, not a second
scan**.

**Design.**

1. Replace `Gc::changedShardCount(const GcState &) -> size_t` with
   `Gc::preFoldRefScan(const GcState &) -> RefScanSummary`, where

   ```cpp
   struct RefScanSummary
   {
       size_t changed_shards = 0;                                 /// unchanged meaning: the defer signal
       std::map<String, std::set<RefTxnId>> logs_by_ns;           /// Log-kind keys only
       std::map<String, RefTxnId> max_log_by_ns;                  /// per-namespace maximum
   };
   ```

   The parse is unchanged (lenient `parseRefObjectKey`, Log kind only). `changed_shards` is computed
   exactly as today; nothing about the defer decision moves.

2. `runRegularRound` keeps the summary and passes it to `fold`. A deferred round discards it.

3. In `fold`, immediately after `groupRefKeys` succeeds and **before** the per-namespace intake loop,
   compare the two derivations per namespace with the rule from {#gaps-are-legitimate}, in both
   directions. Any violation records an anomaly and sets `ref_folding_aborted = true`.

**Why `ref_folding_aborted` and not merely `suppress_destructive`.** The decision text says "suppress
destructive actions for the round and log loudly". Suppression alone is not enough here: the cursor
advance is the *permanent* half of the damage — a record below the sealed cursor can never be
recovered by a later complete page. `ref_folding_aborted` (`CasGc.cpp:1224-1239`) already does exactly
the right thing and nothing more: it discards every accumulated delta, carries each table's parent
cursor verbatim into the new seal, and leaves the recorded anomaly to drive `suppress_destructive`
(`:1373`). Reusing it means probe A adds no new failure mode, only a new reason to enter an existing
one.

**What probe A catches:** any cause of a hole in either enumeration — a backend returning an
incomplete page, continuation behaviour under concurrent mutation, a bug in or below
`forEachListedKey`, a mis-parse. Mechanism-agnostic by construction.

**What probe A misses:** a hole that is *deterministic* and reproduces identically in both
enumerations. Nothing cheap catches that; it is a limitation to state, not to paper over.

**Cost:** zero additional backend calls. One `std::set<RefTxnId>` per namespace held for the duration
of the round — bounded by the number of ref-log objects, which the round already materialises as a
`std::vector<String>` of full keys (`ref_object_keys`, `:842`), an order of magnitude larger.

### Probe B — "we dropped it", and why the naive version is vacuous {#probe-b}

The naive reading of "count records intended-to-fold versus actually-applied per round and require
equality" is a trap. In the current control flow, `intended` and `applied` would be incremented in the
same basic block: a clamp `break`s out and never reaches the cursor advance, so the two counters are
equal **by construction**. That is precisely the failure mode the S42 soundness guard fell into
(item 3 of this document) — a counter that cannot differ is not a detector.

For probe B to mean anything, the two counts must be taken at points that a real defect can separate.
There are two such separations, and they catch different things:

**B1 — the intake-layer identity (cheap, control-flow only).** At seal time, recompute the intended
count from independent data: for each namespace, the number of `listing.logs` ids in
`(cursor_prev, cov.last_folded_ref_id]`. Compare with a running counter incremented at the single site
`resolved_through = log_id` (`:1210`). These are derived differently (a recomputation from the sealed
coverage vs. a running counter), so a future refactor that advances a cursor without folding breaks
the equality. **Be honest about its reach:** the recomputation reads the SAME listing, so B1 is blind
to a record missing from the listing. It is a control-flow assertion, not a detector for the suspected
defect. It costs nothing and it is worth keeping for exactly that reason.

**B2 — end-to-end transaction accounting (the half that matters).** Prove that every transaction the
round declares covered, and which produced at least one blob delta, had at least one of its deltas
reach a shard reducer.

```cpp
/// Round-local, never persisted. Index space = the order in which logs were opened this round.
struct TxnApplyLedger
{
    std::vector<RefTxnId>  txns;        /// ordinal -> the log id
    std::vector<String>    namespaces;  /// ordinal -> namespace, for the failure message
    std::vector<uint8_t>   produced;    /// this txn emitted >= 1 BlobDelta
    std::vector<uint8_t>   committed;   /// this txn folded fully and merged into the round buffers
    std::vector<uint8_t>   applied;     /// >= 1 of this txn's deltas was consumed by a reducer

    uint32_t open(const RootNamespace & ns, const RefTxnId & id);   /// returns the ordinal
    void markProduced(uint32_t ordinal);
    void markCommitted(uint32_t ordinal);
    void markApplied(uint32_t ordinal);
    /// Ordinals with committed && produced && !applied. Empty on a healthy round.
    std::vector<uint32_t> unapplied() const;
};
```

- `BlobDelta` (`Gc/CasBlobInDegree.h`) gains `uint32_t txn_ordinal`.
- `Gc::foldManifestEdges` takes the ordinal and stamps every delta it pushes; the caller calls
  `markProduced` when the per-log staging buffer is non-empty.
- `markCommitted` fires at the existing merge point (`:1200-1211`).
- `foldDeltasIntoGeneration` and `ShardReducer::reduce` take a `TxnApplyLedger *` and call
  `markApplied(d.txn_ordinal)` for each delta they iterate. Both run sequentially on the fold thread
  (`CasGc.cpp:1398`, `:1422-1443`), so no synchronisation is needed — state that in the header so a
  future parallel reducer has to confront it.
- At the end of `fold`, before `putDeterministicArtifact` of the seal, `unapplied()` must be empty.

**Why marking at reducer *consumption* and not at run flush.** The in-degree model is a SET, so a
legitimate unmatched `-1` and a duplicate `+1` both vanish inside the reducer without reaching the
run. Marking at flush would fire on healthy rounds. Marking at consumption proves routing and reducer
entry, which is the seam the decision named ("delta routing across gc shards, the reducer"). Loss
*inside* the reducer's own set collapse is a different class, covered by the existing
`CasGcUnmatchedRemoveDeltas` signal and by the mirror safety test below; probe B does not claim it.

**Response on violation:** `throw Exception(ErrorCodes::CORRUPTED_DATA, …)` naming the unapplied
`(namespace, txn_id)` pairs, thrown before the seal write and therefore long before the single
`gc/state` CAS. Nothing is adopted; the round evaporates; the scheduler logs a Failed round and
retries next tick.

This is deliberately harsher than probe A's suppression, and deliberately different from
[[feedback_ca_gc_never_throw_on_404]]. A 404 during a fold is missing evidence and must never wedge
the round. An unapplied committed transaction is not missing evidence — it is proof that this round
lost a durable record it had already read and decoded. Wedging GC there is the fail-CLOSED direction:
GC reclaims nothing and deletes nothing until an operator intervenes. The documented escape is
`SYSTEM CONTENT ADDRESSED GC REBUILD`, and the throw message must say so, matching the existing
fail-closed throws at `:892` and `:1073`.

**Cost:** one `uint32_t` per `BlobDelta` and four `std::vector<uint8_t>` sized by the number of logs
folded this round. Whether the `uint32_t` is free depends on `BlobDelta`'s current padding — measure
before assuming (`.claude/tools/cppexpr.sh`), and if it grows the struct, say so in the commit message
rather than discovering it in a perf run.

### The mirror safety test {#mirror-test}

Independent of both probes, and valuable regardless of which fix eventually lands: pin the DELETION
direction of the defect *before* the protocol is rewritten.

A `CasInMemoryBackend` subclass whose first `list` call over the ref prefix filters out one chosen
key, while exact `get` of that key still succeeds. Two tests:

1. **Retention direction** (the RCA's primary reproduction): seed `A` (owner-add), `R` (its
   owner-remove), `H` (a later harmless record); omit `R` from the first ref-prefix `LIST`. Assert the
   defect: the cursor advances to `H`, a residual `+1` survives, and restoring the listing does not
   cancel it.
2. **Deletion direction** (the mirror): omit a second live owner's `+1`, fold the first owner's `-1`,
   drive graduation across rounds, and assert the live blob is **never** deleted.

**Order matters and is the point.** Write both tests first and watch them go RED against the shipped
code — that converts the TLA+ result into an executable one. Only then land probe A, after which both
go GREEN because the round aborts ref folding instead of advancing. **If a test does not go red before
the fix, that is a finding**: the assumed mechanism does not reach the shipped code by the assumed
route, and the detector's target has to be re-derived. Do not adjust the test until it goes red.

### Open questions {#item-1-open}

- Probe A's independence rests on the two enumerations being separate physical walks. If a future
  optimisation merges them (an obvious and otherwise-good idea — the round lists the same prefix
  twice), probe A silently becomes vacuous. It needs a guard: either a comment that forbids the merge,
  or a test that asserts two distinct `LIST` walks occur. Which?
- Probe B's throw wedges GC until an operator runs the rebuild. On a pre-release soak stand that costs
  a disk-budget blowout (the last run reclaimed 124 GB at one checkpoint). Acceptable, or should the
  first N occurrences suppress-and-continue with a counter, escalating to a throw only on recurrence?
- `BlobDelta` growth: unmeasured at design time.
- Neither probe covers a *deterministic* hole reproduced identically by both enumerations. Is that
  worth a third probe (a per-namespace re-`LIST` of `cas/refs/<ns>/` for advancing namespaces, at one
  extra `LIST` per advancing namespace), or is it left to the eventual chain fix?

---

## Item 2 — one row per GC phase {#item-2-phase-rows}

The shape is the user's, verbatim: **each GC phase emits its OWN ROW** in
`system.content_addressed_garbage_collection_log`, carrying the metrics that matter for that phase —
not extra columns on the single per-round row.

Today the table gets exactly two rows per round: a `Start` and a `Finish`
(`Gc/CasGcScheduler.cpp:136-142`, `:160-195`), and the GC has no timing instrumentation whatsoever —
`grep -iE "Stopwatch|elapsed"` over `Gc/*.cpp` returns nothing. The `Finish` row's `duration_ms` is
the only wall-time number in existence, and it covers the whole round.

### The phases, enumerated from the code {#phases}

Read out of `Gc::runRegularRound` (`CasGc.cpp:269-751`) and `Gc::fold` (`:833-1489`), in execution
order. This supersedes the candidate list in BACKLOG {#gc-bottleneck-study-2026-07-25}, which was a
guess.

| # | `phase` | Code | Dominant I/O | `phase_metrics` keys |
|---|---|---|---|---|
| 1 | `lease` | `acquireOrRenewLease` (`:274`) | `gc/state` GET + CAS | `steal_attempted`, `acquired` |
| 2 | `heartbeat_floor` | `computeHeartbeatFloor` (`:306`) | LIST `gc/server-roots/` + GET per mount + fence PUTs | `live`, `terminated`, `fenced_now`, `already_fenced` |
| 3 | `defer_decision` | `graduationDue` (`:348`) + `preFoldRefScan` (`:349`) + `shouldDeferRound` | **one full `cas/refs/` LIST** + one fold-seal GET | `changed_shards`, `ref_keys_listed`, `namespaces_seen`, `graduation_due`, `deferred` |
| 4 | `fold_ref_list` | `fold` step 1 (`:842-877`) + probe A | **one full `cas/refs/` LIST** | `ref_keys_listed`, `namespaces_seen`, `probe_a_holes`, `probe_a_deletions` |
| 5 | `fold_seal_read` | `readFoldSeal` (`:890`, `:1037`) | fold-seal GET (see the note below) | `seal_bytes`, `parent_runs`, `ns_cleanup_items` |
| 6 | `fold_ref_intake` | the per-namespace loop (`:1049-1219`) | one GET per new log + one GET per manifest edge | `logs_intended`, `logs_applied`, `edges_emitted`, `deltas_emitted`, `clamps`, `dead_precommits_skipped` |
| 7 | `fold_reduce` | `foldDeltasIntoGeneration` / `ShardReducer::reduce` (`:1398`, `:1432`) | prior-run streaming GETs, one HEAD per zero-transition candidate, run PUTs | `shards_reduced`, `shards_pure_carry`, `deltas_in`, `candidates_headed`, `condemned`, `graduated`, `spared`, `run_rows_written`, `txns_unapplied` |
| 8 | `fold_ns_cleanup_scan` | `namespacePhysicallyEmpty` (`:1259-1294`) | two LISTs per pending item | `items_carried`, `items_completed`, `items_retired`, `prefix_lists` |
| 9 | `fold_seal_write` | `putDeterministicArtifact` (`:1479`) | one PUT (or a byte-compare GET) | `seal_bytes` |
| 10 | `pending_deletes` | the pre-CAS loop (`:424-612`) | one `deleteExact` per pending entry (+ a HEAD on the 412-on-absent quirk) + one outcome-log PUT per shard | `redeleted`, `deleted`, `absent`, `replaced`, `spared`, `quirk_heads` |
| 11 | `meta_pool_wait` | `meta_pool->wait()` (`:619`) | none on this thread — pure queue drain | `jobs_scheduled`, `jobs_completed` |
| 12 | `round_commit` | `pruneSupersededGenerations` + the single `gc/state` CAS (`:645-651`) | prune LISTs + deletes, one CAS | `generations_pruned`, `objects_pruned`, `pruned_through` |
| 13 | `handoff_reclaim` | the post-CAS hand-off block (`:675-697`) | `deletePrefixWholesale` | `generations_reclaimed`, `objects_reclaimed` |
| 14 | `manifest_deletes` | the `mf_cleanup` loop (`:702-720`) | one `deleteExact` per manifest | `attempted`, `deleted`, `absent`, `replaced` |
| 15 | `namespace_cleanup` | `runNamespaceCleanupPasses` (`:736`) | snapshot republish + prefix deletes | `items_pending`, `items_completed`, `objects_deleted` |
| 16 | `ref_object_cleanup` | `cleanupRefObjects` (`:738`) | one HEAD + one `deleteExact` per deletable ref object | `logs_deleted`, `snapshots_deleted`, `suppressed` |
| 17 | `orphan_sweep` | `runManifestSweepCursorPass` (`:743`) | budgeted LIST + deletes | `listed`, `deleted`, `skipped`, `cursor_advanced` |

Two things the enumeration itself surfaced, recorded here because they are cheap findings and the
instrumentation will make them visible anyway:

- **The round lists `cas/refs/` twice** (phases 3 and 4). This is what makes probe A free. It is also
  a real cost at the CI scale that produced the throughput collapse, and the measurement campaign will
  now show it as two separate rows instead of one opaque blob.
- **The adopted fold seal is read twice** in `fold`: `adopted_seal` (`:890`) and `discover_ref_seal`
  (`:1037`) are the same key at the same `(generation, attempt)`. One redundant GET per round. Not
  fixed here — instrumented, so the follow-up study can decide.

### Row shape {#row-shape}

Schema additions to `ContentAddressedGarbageCollectionLogElement`
(`src/Interpreters/ContentAddressedGarbageCollectionLog.{h,cpp}`):

- `event_type` gains `PHASE = 3` (`Enum8('Start'=1,'Finish'=2,'Phase'=3)`).
- `round_id` `String` — **the correlator**. A fresh `UInt128` minted per `runRoundLogged` invocation,
  rendered hex, stamped on the `Start` row, every `Phase` row, and the `Finish` row. This is what a
  reader groups by. It is not the round number: `round` is `0` on `Start`, is only known after the
  single `gc/state` CAS on a folding round, and does not exist at all on a `NotALeader` round — a
  correlator has to work for the rounds that fail, which are the interesting ones.
- `phase` `LowCardinality(String)` — one of the 17 names above; empty on `Start`/`Finish`.
- `phase_duration_us` `UInt64` — monotonic wall time for this phase. Microseconds, not milliseconds:
  `meta_pool_wait` and `round_commit` are routinely sub-millisecond and the whole point is to see when
  they are not.
- `phase_metrics` `Map(LowCardinality(String), UInt64)` — the per-phase semantic counts from the table
  above.

**No new verb columns.** The existing `ProfileEvents` `Map` column carries the per-phase delta on a
`Phase` row, and it already contains `S3GetObject` / `S3PutObject` / `S3HeadObject` / `S3ListObjects`
/ `S3DeleteObjects` / `DiskS3*` plus every `Cas*` counter. `GROUP BY phase` over
`ProfileEvents['S3ListObjects']` answers "which phase burns the LIST budget" with no schema invention.
On the `Finish` row that column keeps its current meaning (the whole-round delta), so nothing existing
changes meaning.

**How a reader correlates one round:**

```sql
SELECT phase, phase_duration_us, phase_metrics, ProfileEvents['S3ListObjects'] AS lists
FROM system.content_addressed_garbage_collection_log
WHERE round_id = '…' AND event_type = 'Phase'
ORDER BY event_time_microseconds;
```

and across rounds, the round-time split the study needs:

```sql
SELECT phase,
       count() AS rounds,
       quantile(0.5)(phase_duration_us) AS p50,
       quantile(0.99)(phase_duration_us) AS p99,
       sum(phase_duration_us) AS total_us
FROM system.content_addressed_garbage_collection_log
WHERE event_type = 'Phase' AND disk_name = 'ca'
GROUP BY phase ORDER BY total_us DESC;
```

### How the numbers are collected {#collection}

A small RAII timer owned by `Cas::Gc`, emitting through a sink the scheduler installs:

```cpp
/// One phase of one round. Pure data, no Interpreters dependency (same discipline as GcRoundLogRecord).
struct GcPhaseRecord
{
    String phase;
    UInt64 duration_us = 0;
    std::map<String, UInt64> metrics;
    std::map<String, UInt64> profile_events;   /// this phase's delta
};
using GcPhaseSink = std::function<void(const GcPhaseRecord &)>;
```

The timer takes `CurrentThread::getProfileEvents().getPartiallyAtomicSnapshot()` at entry and exit and
subtracts. It deliberately does **not** use a nested `ProfileEventsScope`: that re-parents the
thread's counters (`ProfileEventsScope.cpp:12`), and the round-level scope installed at
`CasGcScheduler.cpp:149-151` is already holding that slot. A plain snapshot diff of whatever container
is currently attached composes with the outer scope instead of fighting it, and degrades to an empty
map on a bare gtest thread with no `ThreadStatus` — the same degradation the round-level capture
already accepts and documents.

**Always on.** No setting. The per-phase cost is one `steady_clock::now()` and one counters snapshot
against phases that each perform network I/O. A setting here would recreate the exact failure this
week produced three times over — a knob whose default nobody remembers, degrading to silence.

**One honest gap: the meta pool.** `scheduleMetaJob` work runs on `meta_pool` threads, so their
ProfileEvents do not appear in the fold thread's snapshot, and phase 11 (`meta_pool_wait`) will show a
duration with no verb counts. That is exactly the seam defect 1 of the RCA predicts the fold thread
parks on, so it must not be left as an unexplained blank: phase 11's `phase_metrics` carries
`jobs_scheduled` / `jobs_completed` counted by `Gc` itself at the schedule and completion sites, which
is enough to distinguish "the queue was deep" from "the endpoint was slow" when read next to the
duration.

**Volume.** 17 rows plus 2 per round. At `gc_interval_sec=5` that is ~3.2 rows/s per disk, ~275k rows
per disk per 24 h soak. Negligible for a `SystemLog`, and the sink is already best-effort
(`CasGcScheduler.cpp:122-134` swallows a throwing logger; `ContentAddressedMetadataStorage.cpp:517`
drops on a full queue).

### Where item 1's probes live in this {#probes-in-rows}

Designed together, not twice:

- probe A's outcome is `phase_metrics['probe_a_holes']` and `['probe_a_deletions']` on the
  `fold_ref_list` row, plus the existing anomaly channel;
- probe B1's two numbers are `phase_metrics['logs_intended']` and `['logs_applied']` on the
  `fold_ref_intake` row — visible on every healthy round, so "they are always equal" becomes an
  observable property rather than an assumption;
- probe B2's verdict is `phase_metrics['txns_unapplied']` on the `fold_reduce` row (0 on a healthy
  round; a nonzero value is accompanied by the throw, so the row is the forensic record of a round
  that then failed).

### Open questions {#item-2-open}

- Should `Start`/`Finish` stay as they are, or should `Finish` become derivable from the phase rows?
  Keeping them is backward-compatible and cheap; the redundancy is a small cost.
- 17 phases is a judgement call. `fold_seal_read` and `fold_seal_write` are one GET and one PUT and
  could fold into their neighbours; `round_commit` mixes the prune (heavy) with the CAS (trivial) and
  arguably should split.
- `phase_metrics` as a `Map` means no type checking on key names. A typo is invisible. Worth a
  compile-time key list, or is that over-engineering for an introspection table?

---

## Item 3 — what "green" means for S42 {#item-3-s42}

**Decision:** green is *a consistent state on disk and in memory*, not proof that a fault landed in
the post-durable install window. Consistency assertions decide; targeted counters stay reported; the
anti-vacuity guard survives on the GENERIC fault count.

### Why the card is inconclusive by construction today {#s42-today}

`utils/ca-soak/scenarios/cards/s42_alloc_faults.py` never computes an overall verdict — the harness
takes the worst of the `Verdict` rows (`framework/report.py:20-21`, `_RANK = {pass:0, skipped:1,
inconclusive:2, fail:3}`). Two rows cap every healthy run:

1. `:557-565` — the soundness guard: `targeted == 0` emits `Verdict.inconclusive`. `targeted =
   poison_total + failpoint_hits`, and `failpoint_hits` is the literal `0` returned by
   `_post_put_failpoint_hits()` (`:138-148`) because the install-region seam is the gtest-only
   `CasRefLedger::setInstallRegionProbeForTest` with no `src/Common/FailPoint.cpp` registration, while
   `poison_total` is correctly 0 whenever §A1 holds. Zero by construction, from both terms.
2. `:517-521` — `poison_total == 0` emits `Verdict.skipped` for "no snapshot advanced across a
   poisoned transaction". `SKIPPED` ranks **above** `PASS`.

**Fixing (1) alone leaves the run reading `skipped`, not `pass`.** The decision text does not mention
this second row; it must move too, or the rework does not achieve what it was asked to achieve.

### The rework {#s42-rework}

1. **Add a non-gating verdict factory.** `framework/report.py` has `check` / `inconclusive` /
   `skipped` and no way to record an observation without affecting the status, which is why the card
   hand-constructs `Verdict(..., "pass")` twice (`:673-684`). Add:

   ```python
   @staticmethod
   def reported(name: str, expected: str, observed, note: str = "") -> "Verdict":
       """A recorded observation that never gates the run status. Use ONLY where the metric is
       genuinely non-gating by design; never as a way to soften an assertion that should fail."""
       return Verdict(name, expected, str(observed), PASS, note)
   ```

2. **Soundness guard → reported.** Both branches of `:557-572` become `Verdict.reported`, with the
   note preserving the full honesty of the current text: a zero still means the target window was not
   proven traversed, it just no longer decides the run.

3. **`:517-530` → reported on the zero branch.** The `poison_total > 0` branch stays a real `check`
   (if poison ever fires, the snapshot oracle must be clean).

4. **Generic anti-vacuity survives unchanged** (`:543-555`): `generic == 0` stays
   `Verdict.inconclusive`. A run in which no allocation fault occurred at all still cannot read green.
   This is the part of the discipline the decision explicitly preserved.

5. **Retro-fit the two hand-built `Verdict(..., "pass")` rows** at `:673-684` to `Verdict.reported`.
   Same behaviour, one convention.

6. **Rewrite the docstring** (`:38-55`). It currently states as fact that "S42 can only return
   `inconclusive` … It cannot return a conclusive green". After this change that is false, and a stale
   docstring asserting a stronger claim than the code makes is exactly the failure
   [[feedback_docs_write_for_future_readers]] is about. The replacement states: green means the
   consistency oracle held under a run that provably injected allocation faults; the targeted window
   remains unproven and is reported, not gating.

Everything the verdict now rests on **already exists** in the card and needs no new assertion:
post-restart view identical to pre-restart (`:402`, `:420-433`), every acked block present after the
journal rebuild (`:435-444`), replicas agree (`:446-447`), fsck `dangling`/`unaccounted`/`stale_edge`
clean pre- and post-restart (`:456-473`), the snapshot integrity oracle (`:475-495`), zero
`LOGICAL_ERROR` (`:605-616`), no wedged ref lane (`:641-644`), GC rounds succeed after disarm
(`:658-668`), plus `_common.standard_end`'s common assertions (`:686`).

### Deliberately NOT done {#s42-not-done}

Named so the next reader knows they were considered and rejected as scope inflation, not missed:

- The acked-block check runs only against `node.container` (`:437`), not per replica. Replica
  agreement is separately asserted, which covers it transitively.
- `_events` returns `{}` on a query failure (`:105-106`), so a broken probe reads as zero — which
  makes `generic` zero, which yields `inconclusive`. Fail-safe already.
- `poison_log_lines == -1` (a failed `text_log` probe) is printed but not detected (`:505`, `:511`).
  Cosmetic; corroboration only, never gating.
- `fsck_pre` and `fsck_post` are each asserted clean but never compared structurally, despite the
  comment at `:449` promising a comparison.

### Open questions {#item-3-open}

- With S42 able to read green, does it become a soak gate (a red blocks the branch) or does it stay
  advisory? The card sets `expect_exception=True` and its whole design assumes queries fail.
- Should `Verdict.reported` be usable at all, given the README rule that "an assertion whose data is
  unavailable must be recorded as `inconclusive`, never dropped" (`report.py:7-8`)? The distinction is
  *unavailable data* (inconclusive) vs *a metric that is not an assertion* (reported), and the
  docstring above draws it — but it is a new escape hatch in a framework built to prevent exactly
  that, and it will be reached for again.

---

## Item 4 — force-claim at mount {#item-4-force-claim}

**Decision:** the problem is the differing SERVER UUID, not `mountWritable`. The fix is a mount-time
"never mind the uuid mismatch, force a new one" that mounts as **WRITE**. A true read-only mount is a
separate, unimplemented task and is not designed here.
`docs/superpowers/specs/2026-07-25-cas-tool-read-without-ownership-design.md` is superseded as a
recommendation; its verified facts stand and are used below.

### The one check the BACKLOG asked for: the CI scrape is POST-MORTEM {#ci-scrape-check}

Established from the job definitions, not from the plan text.

- The scrape is `ClickHouseProc.dump_system_tables`
  (`ci/jobs/scripts/clickhouse_proc.py:1251`), whose own comment says: *"Stop server so we can safely
  read data with clickhouse-local. … Because it's the simplest way to read it when server has
  crashed."*
- The ordering is unconditional in the job flow: `CH.terminate()` at
  `ci/jobs/functional_tests.py:1034`, log collection at `:1077-1081`, and `terminate()` itself runs
  `clickhouse stop --pid-path … --max-tries 300 --do-not-kill` (`clickhouse_proc.py:967`) escalating
  to `SIGTRAP` and `proc.kill()` (`:974`, `:978`).
- `clickhouse_proc.py:1290-1291` already says it in words: *"The server is already stopped, so
  `clickhouse local` opens the pool under its own (unrelated) identity."*
- Same machine, same data directory, same config tree (`--path {run_path0}` at `:1328-1332`,
  `--config-file=/etc/clickhouse-server/config.xml` at `:1282`).

**So the concern is void for the CI case, and I am saying so plainly rather than designing around a
risk that is not there.** Three caveats that are real and are part of the design's premises:

1. The scrape is **conditional on failure**: `functional_tests.py:1081` passes
   `all=test_result and not test_result.is_ok()`, and `clickhouse_proc.py:1005-1007` gates
   `dump_system_tables()` on it. A green job never scrapes.
2. "Stopped" is best-effort. `terminate()` does not assert the pid is gone before returning, and
   `prepare_logs` swallows exceptions. A wedged server is a narrow window in which the scrape could
   overlap a live process. **The guard below must therefore be real, not decorative** — the design
   cannot rest on the CI ordering being perfect.
3. There is **no CAS-tool scrape in CI at all**. No `ca-fsck` / `ca-inspect` / `ca-gc-*` step exists
   under `ci/`, `.github/`, `tests/docker_scripts/`, or `docker/`. The pool is opened only
   incidentally, because initialising the disks initialises all of them. The live-server CAS tool
   usage is in integration tests and the soak stand, and all of those go through a `<readonly>true</readonly>`
   shadow disk (`tests/integration/test_content_addressed_ref_snaplog/configs/storage_conf.xml:22-32`).

And a finding the user should weigh before this ships: the CI carve-out from `ee15c8ade23` is still
dead code. Its `sed` patches `/etc/clickhouse-server/config.xml` (`clickhouse_proc.py:1298-1300`),
which contains no `<metadata_type>` tag at all, while the CA disk is installed into `config.d` as a
symlink (`tests/config/install.sh:382`, `:391`; the marker is at
`tests/config/config.d/content_addressed_s3_storage_policy_for_merge_tree_by_default.xml:7`).
**Fixing that one line makes the CI scrape work today with zero product change**, because the
`<readonly>` disk path already skips `mountWritable` entirely (`CasPool.cpp:459-461`). That does not
overrule the decision — force-claim is still the general answer for an operator, and read-only is a
separate task — but it means the CI motivation for shipping force-claim is weaker than the plan text
assumed, and the one-liner should land regardless.

### Why forcing the owner alone does not work {#force-two-objects}

`mountWritable` (`CasPool.cpp:466`) runs four bootstrap-control steps. The uuid appears in **two**
durable objects, and the refusal fires from both:

1. `claimOwnerOrThrow` (`Pool/CasServerRoot.cpp:108`) — the owner anchor. A foreign
   `server_uuid` throws `CORRUPTED_DATA` at `:125-131`. This is the refusal the decision is about.
2. `claimMount` (`:374`, contract at `Pool/CasServerRoot.h:305-330`) — the mount lease. Its rule is
   explicit: *"different `server_uuid` → `ForeignOwner` (do NOT write, regardless of expiry or prior
   state)"*.

A graceful shutdown does **not** delete the mount object; `MountLeaseKeeper::terminate` stamps the
farewell sentinel (`min_active == UINT64_MAX`) into it and leaves it in place, carrying the
predecessor's uuid. So a force that rewrites only the owner anchor then hits `ForeignOwner` on step 2
and fails anyway — after having permanently rewritten the pool's identity. Any design that stops at
the owner anchor is worse than no design.

The mount slot is also where the *liveness* protection lives (the owner anchor is identity, and
clock-free). So the guard the BACKLOG entry asks for is not an add-on: it is the mount-slot half of
the same change.

### Design {#force-design}

**Surface.** A per-disk, opt-in CAS setting, never a default:

```xml
<force_owner_claim>true</force_owner_claim>
```

declared in the `DECLARE` list at `ContentAddressedSettings.cpp:76-83` alongside `skip_access_check`,
read into `ContentAddressedMetadataStorage` next to it (`ContentAddressedMetadataStorage.cpp:286`),
and carried into `Cas::PoolConfig::force_owner_claim` (`Pool/CasPool.h`, default `false`) at
`ContentAddressedMetadataStorage.cpp:711-713`. Config-level rather than a CLI flag because the pool is
opened by disk initialisation, not by an applet — which is exactly why the scrape hits it at all.

**Precondition gate (the guard, non-negotiable).** Before *any* write, prove the foreign mount slot is
not live. A new `Pool/CasServerRoot` helper:

```cpp
/// Evidence that a FOREIGN-uuid mount slot may be taken over. Mirrors `claimMount`'s
/// certificate-of-death discipline (see its doc comment): a bare `expires_at_ms <= now_ms` reading is
/// NEVER sufficient, because comparing a predecessor's stamp against our wall clock is unsafe.
struct ForeignMountDeath
{
    enum Kind { Absent, CleanFarewell, GcFenced, ObservedStable, Live };
    Kind kind = Live;
    std::optional<Token> token;   /// set for CleanFarewell / GcFenced / ObservedStable
    MountLease body;
};

ForeignMountDeath proveForeignMountDead(
    Backend & b, const Layout & l, const String & srid,
    const std::function<uint64_t()> & mono_ms_fn, uint64_t ttl_ms, uint64_t poll_interval_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn);
```

`Absent`, `CleanFarewell` (`min_active == UINT64_MAX`) and `GcFenced` decide immediately — the common
case, including a cleanly stopped server, costs one GET. Anything else enters the same
token-stability observation the codebase already uses (`mountObservationThresholdMs(ttl_ms,
poll_interval_ms)`, `CasServerRoot.h:412`), bounded by the same restart limit: a token that keeps
changing means a live holder, and the result is `Live`. That path costs up to one observation window
(~TTL + 5% + one poll) and is the case that matters for a *crashed* server — which is precisely when
CI scrapes.

`Live` ⇒ `throw Exception(ErrorCodes::ABORTED, …)` naming the holder, **having written nothing**.

**Order of writes.** Load-bearing, because two of the orderings are unsafe:

1. `proveForeignMountDead` — read-only.
2. Overwrite the owner anchor with our uuid, conditional on the token read in step 1's owner GET (a
   `casPut`, so a concurrent legitimate claim wins and we fail).
3. `allocateWriterEpoch` — unchanged. It CAS-bumps the durable monotone `epoch` object, which is
   per-`srid` and not per-uuid, so the counter is preserved across the takeover. **Do not delete the
   mount object before this step**: the absent-epoch branch refuses to re-mint `writer_epoch 1` while
   a mount object exists (`CasServerRoot.cpp` mount probe, commit `6094c1473ea`), and removing the
   mount first would disarm that guard.
4. `deleteExact(mountKey, token from step 1)` — token-conditional, so a predecessor that renewed
   between the observation and here causes a miss and we refuse. Skipped when step 1 returned
   `Absent`.
5. `claimMount` — now sees an absent slot, `putIfAbsent`, `Claimed`. **`claimMount` itself is not
   modified**; the whole force concept stays inside `mountWritable`, and the `ForeignOwner` invariant
   in the safety-critical primitive is untouched.

**Loudness.** A `LOG_WARNING` at open naming the displaced uuid, the new uuid, and the death
certificate used; plus a `CasEvent` audit row carrying the same three fields, so
`system.content_addressed_log` records who took the pool and on what evidence.

### The consequence that must be said out loud {#force-consequence}

A force-claim **permanently reassigns the pool's identity**. The original server, on its next start,
presents the old uuid, is refused by `claimOwnerOrThrow`, and cannot mount its own root without manual
recovery (restore the owner object, or reconfigure `server_root_id`). For a CI container that is
discarded a minute later this is free. For a soak stand or a developer's pool it is a footgun, and the
error message the original server gets (`CasServerRoot.cpp:125-131`) blames a regenerated local uuid
file, which will be the wrong diagnosis.

**There is a cheaper alternative that the codebase already implements**, and I am recording it rather
than quietly preferring it, because the decision's wording ("force a new one") reads as the overwrite:
`Pool::openForDecommission` (`CasPool.cpp:720-776`) **impersonates** — it reads the existing owner uuid
(`readOwnerUuid`) and mounts as it, so `claimOwnerOrThrow` passes trivially, `claimMount` sees a
same-uuid/different-epoch slot and applies the existing certificate-of-death reclaim, and the pool's
identity is preserved so the original server can come back. It reaches the same "mount as WRITE
despite a differing uuid" outcome with strictly less durable damage and less new code. Its cost is
semantic: two processes then present the same owner uuid, which is exactly what the mount lease exists
to serialise, and it makes the tool indistinguishable from the server in the owner object.

This is the first open question below, and it should be settled before the plan's item-4 tasks run.

### Open questions {#item-4-open}

- **Overwrite or impersonate?** The literal reading of the decision is overwrite; impersonation is
  cheaper, reversible, and already implemented for decommission. The plan implements overwrite and
  isolates the choice to one function so the switch is a small edit — but it is a user decision, not
  an implementation detail.
- Should the fix to the CI `sed` (patch `config.d`, not `config.xml`) land in this batch? It is a
  one-liner, it makes the scrape work with no product change, and it makes the force-claim's CI
  justification moot.
- Force-claim writes on a path whose motivating caller is a **diagnostic**. Should a forced mount also
  refuse to start the GC scheduler, so a tool that takes ownership cannot also start reclaiming? The
  decision says "mounts as WRITE", which argues no; the blast radius argues yes.
- The observation wait (~TTL + 5%) is paid exactly in the crashed-server case. Is a bounded wait
  acceptable inside disk initialisation, which is on the server/tool startup path?
