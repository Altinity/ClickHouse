---
description: 'Running ledger for the ca-soak scenario repair campaign: what is fixed and proven, what is diagnosed but unfixed, and what is untouched.'
sidebar_label: 'Scenario repair ledger'
sidebar_position: 97
slug: /superpowers/cas/scenario-repair-ledger
title: 'ca-soak scenario repair — ledger'
doc_type: 'reference'
---

# ca-soak scenario repair — ledger {#ca-soak-scenario-repair-ledger}

Started 2026-08-31 from a request to pick more realistic thresholds, fix defects, and add
instrumentation. A row is DONE only if a run proves it; a diagnosis is not a fix.

**The campaign's central finding, which reframed it:** of the scenarios that had been unresolvable at
every scale, **not one was blocked by scale**. Thresholds were the symptom. The causes were a counter
that the tested path never increments, a format specifier that made every run throw, a measurement
window that closed before the work began, and four unrelated defects stacked in one card.

## Fixed and proven by a run {#fixed-and-proven}

| # | What was wrong | Evidence it is fixed |
|---|---|---|
| S06 | Verdict gated on `CASBlobGet`, which column-data reads never increment — inert at every scale | **PASS 10/10** (was INCONCLUSIVE at dev, ci and full) |
| S15 | `formatDateTime(now(),'…%H:%M:%S')` — in ClickHouse `%M` is the MONTH NAME, so every query scoped by that timestamp threw `Code: 41` and killed all three variants | **PASS 11/11** (was 5 of 8 verdicts unresolved) |
| S21 | Four stacked defects: inert counter; 1.97 TB `full`; `readers: 16` saturating RustFS; merges collapsing the 100 parts that ARE the premise, then the end checkpoint forcing the same merge via `OPTIMIZE FINAL` | **PASS 13/13**, verdict now separates subset from full scan 59-fold (`1col=199 all=11841`, ratio 0.0168 vs the 1/60 predicted) |
| S20 | Counter window opened after `wait_healthy`, but the follower's whole fetch and relink completes in **20 ms** inside it | **PASS 12/12**, `CASRootCompareSwap=6` and 37 counters moving where the delta had been empty |
| S27 | LIST anomalies armed on `cas/refs/`, a prefix that exists nowhere in the layout | Retargeted to `/cas/ns/stream/` (`Layout::casRefsPrefix`). **Not yet run** — needs the `s3listproxy` compose variant |
| Instrumentation | `Memory` trace type never dumped; `total_memory_profiler_step` defaulted to 0, so idle allocations were unsampled | Verified live: `changed = 1`, **1,064 background samples** where the count would have been zero, dump wrote 1.27 MB of stacks |
| RustFS ceiling | A 1.1 GiB merge never completed at the default disk-read concurrency cap | `RUSTFS_OBJECT_MAX_CONCURRENT_DISK_READS=256`: same merge completes, `MergeParts` 1,211.7 s, `error = 0` |

**Sweep completed:** every verdict gating on a `CAS*` counter was checked. 52 counters, 289
references, 52 gating uses. No counter is a ghost — all exist and all have real increment sites. The
defect class is narrow and now closed: only the 14 backend-table counters can be inert, and only on
the one path that bypasses `CasInstrumentedBackend` (column-data reads via `readBlobPayload` →
`object_storage->readObject`). No verdict gates on `CASBlobGet`/`CASBlobGetStream` any more.

## In progress {#in-progress}

**S10** — three claims tested, all three refuted, card rewritten:

- "lightweight `DELETE` is unreliable on this build (CA storage path diverges)" — **false**. Oracle
  test: 50,000 rows, two `DELETE FROM`, expected 40,000 survivors with a given checksum; got exactly
  that, no error. The claim had stood since 2026-07-01 with no recorded error text or reproduction.
- "ALTER DELETE is needed for correct oracle semantics" — **false**. The oracle compares
  `SELECT count()`, which honours the delete mask.
- Patch parts absent because of the setting level — **wrong cause** (mine). The real cause was
  `lw_supported = False` hardcoded, making the lightweight `DELETE` beneath it dead code.

Changes made: `lw_supported = True`; the table now carries `enable_block_number_column`,
`enable_block_offset_column` and `apply_patches_on_merge` (all three are TABLE settings, each named
by the server in a `Code: 48` until supplied); a lightweight `UPDATE` step added, since lightweight
DELETE produces no patch part and UPDATE is the only thing here that does; the probe no longer
`SET`s a table setting and no longer reports support on the strength of any single acceptance.

First run after enabling lightweight DELETE: **INCONCLUSIVE 11/12** — and two prior FAILs cleared
(`no unbounded leftovers` and `obsolete patch content reclaimed` both 0, 0 anomalies), with the
oracle green at 120,000 = 120,000. **Read that cautiously:** the FAILs were seen at `full` and this
run was `ci`, so the honest statement is "not reproduced at ci with lightweight deletes", not
"fixed". A `full` run is required before the leftover-manifest backlog entry can be closed.

## Diagnosed, not yet fixed {#diagnosed-not-fixed}

| # | Diagnosis | What it needs |
|---|---|---|
| S07 | Cap unreachable by scale: `kMaxManifestEntries = 1048576` against tens of thousands of entries from 20,000 columns. The card's own note says a cap-lowering test setting is required | Lower the limit, not raise the workload |
| S29 | The note's advice ("rerun at ci/full") is stale — the run WAS `full` (20M rows) and `non_blob_pool_bytes` was still 1.7 MB | A different lever than scale |
| S01/S02 | `full` is a 100 GiB blob needing 550+ GiB of disk (measured: `[scratch=full-part]` predicted 93 GiB scratch, 108.4 GB observed; `[replicated double-spill]` predicted 186 GiB, 216.6 GB observed) and it filled the disk once | Blob against a lowered `max_memory_usage`; drop the 100 GiB variant |
| S23 | Two verdicts blocked by "compose fixed at 2 servers" though `docker-compose-10replicas.yml` exists. The memory FAIL is 314 MiB tracked growth against a 64 MiB threshold, on an idle EMPTY pool | A fresh-boot 60-minute window read as a curve; and a threshold that accounts for ~176 MB/hour of system-log telemetry |

## Open product questions, kept separate from card repair {#open-product-questions}

- **516 lost `manifest_delete` events.** GC deleted 517 manifests on one node; the CA event log
  recorded one. Emission is unconditional per attempt in `CasGc.cpp` PHASE 15/18, so events are being
  dropped. Until fixed, manifest reclaim cannot be audited from `cas_log`.
- **The client never paces itself.** `s3_max_get_rps = 0` with `s3_retry_attempts = 500`. Raising the
  RustFS ceiling fixed the wedge, but 503s still occur at 256 while the host is entirely idle
  (load 0.82, iowait 0%, NVMe), and a 500-attempt budget turns a persistent refusal into a very long
  wait instead of an error.

## Method note {#method-note}

Half this campaign was spent on wrong explanations that were plausible and cheap to act on: a retry
storm "overwhelming" a store that answers in 1.2 ms; telemetry "explaining" 314 MiB when it accounts
for 44 MiB; a permit leak refuted by the merge completing; three consecutive wrong axes in S21. Each
was refuted by a run, which felt like evidence but was guess-and-check with a 10-to-30-minute
feedback loop.

The S20 case is the one to remember. Two plausible hypotheses — wrong counter, asynchronous
publication — both false; and the first would have produced a **green** verdict, because
`CASRefCheckpointPublished` does move (6). Acting on it would have masked a window defect and looked
like success. What settled it was evidence that *distinguishes* the hypotheses rather than merely
fitting one: part names (`all_0_5_2`, not the fetched `all_0_5_1`) and microsecond timestamps
(publication takes 20 ms, synchronously).

## Status after the second day (2026-09-01) {#status-day-two}

**By variant, because a tally that does not name the variant is not an answer.** The goal is all
scenarios green in all variants; none of the three is there yet, and `ci` is the only one that has
had a complete post-fix pass.

| variant | pass | unresolved | failing | note |
|---|---|---|---|---|
| `dev` | 38 | S01, S04, S07, S23, S29 | S39 | S39 is a run-parameter error of mine, not a defect: the card refuses `--duration 300` because it needs 500 s and says so |
| `ci` | 36 | S07, S29 | **S21** | S11 and S41 were failing here and are fixed and re-verified |
| `full` | S01, S02, S04 | — | **S03, S05** | interrupted at S06 of 44; both failures are new and appear only at this scale |

The unresolved ones at `dev` are mostly physics: S01, S04 and S29 have no premise to test at that size
and are green at larger ones. S07 and S23 are genuinely open and listed below.

## Fixed on day two {#fixed-day-two}

| what | evidence |
|---|---|
| S10 produces patch parts | first time in the card's existence: 3 at `ci`, 4 at `full`, oracle green at 3,000,000 rows |
| S41 | 0 verdicts to **45/45** |
| S11 | the GC-round ceiling now scales with `parts x rows`; PASS at both `dev` and `ci` |
| S01/S02 at `full` | 100 GiB was unrunnable; 8 GiB keeps 64x of headroom over the verdict's noise floor and passes |
| harness cascade | `predown_dump` no longer aborts the scenario it serves; verified in the wild, a 120 s timeout now logs and the run continues |
| `EXPORT PARTITION` on CAS | Altinity#2291, verified end to end with a data oracle |

**Three of day two's four defects were one bug in three files.** A value read from the environment at
import, then used as a DEFAULT ARGUMENT — which binds it a second time, at definition — so a variant
that needs different endpoints could never be honoured however late the variable was set. Found once
by symptom, once by sweeping for module-level `os.environ.get`, and once only because the run stayed
red at 43/45. The search that works is for where a value is BOUND, not where the environment is read.

## Open, in the order the evidence justifies {#open-day-two}

**S21** — the only scenario failing for a reason we have not explained. Hangs 4 of 4 at `ci`, passes at
`dev`. RustFS reports `permits_in_use: 256/256`, 100% queue utilization, answering 503 after ~5 s
while its CPU sits at 0.13%; both a query and the GC thread wait in `poll` on live connections
(545 ESTABLISHED on both sides). `s3_max_connections = 192` cut throttling from 34% to 6.8% and did
not fix the hang. See `[soak-retry-budget-turns-a-503-into-a-livelock]`.

**S03 at `full`** — `Code: 210 mount lease not held`, root-caused to a single unresolved heartbeat
write; see `[mount-renewal-loses-the-lease-on-one-unresolved-attempt]`. Two product concerns in it:
no retry before the lease is surrendered, and one node giving up with 1,969 ms of confirmed budget
left.

**S05 at `full`** — 1,200 standalone repoints; see `[s05-standalone-repoints-on-the-non-transactional-path]`.

**S07 and S29** — both need a limit lowered rather than a workload raised, and neither has been done.
S07's own note says a cap-lowering test setting is required; S29's advice to "rerun at ci/full" is
stale, because the `full` run it recommends still leaves the file too small to attribute RSS.

**S23** — two verdicts blocked by "compose fixed at 2 servers" though a 10-replica compose exists, and
a 64 MiB memory threshold that at this profiling configuration mostly measures ~176 MB/hour of the
server's own telemetry.

**Never run to completion:** the `full` variant. It reached 5 of 44 in two hours before being stopped,
and at that rate would need ~15 hours. Running the whole thing is probably the wrong shape; the
scenarios that are already green at `dev` and `ci` mostly re-prove themselves, while S03, S05, S07,
S21, S29 and the heavy adversarial cards are where `full` earns its cost.
