---
description: 'Status dump of the ca-soak scenario stabilization campaign: what was fixed, what each variant reports, what is filed in the backlog, and what is still unexplained.'
sidebar_label: 'Scenario stabilization status'
sidebar_position: 98
slug: /superpowers/cas/random/scenarios-stabilization-status
title: 'ca-soak scenarios — stabilization and research status'
doc_type: 'reference'
---

# ca-soak scenarios — stabilization and research status {#scenarios-stabilization-status}

Snapshot taken 2026-09-01. Covers two days of work, 26 commits, starting from a request to pick
realistic thresholds, fix card defects and add missing instrumentation.

A line here is only DONE if a run proved it. A diagnosis is not a fix, and a scenario that turns green
because its threshold moved is called out as such.

## The finding that reframed the campaign {#reframing-finding}

The work began from the premise that scenarios were unresolvable because the rig could not reach their
scale. That premise was wrong.

**Of the scenarios that had been unresolvable at every scale, not one was blocked by scale.** The
causes were: a counter the tested path never increments, a format specifier that made every query
throw, a measurement window that closed before the work began, a prefix that exists nowhere in the
layout, and four unrelated defects stacked in a single card. Thresholds were the symptom.

## Status by variant {#status-by-variant}

Reported per variant on purpose. A single pass/fail tally does not answer "are all scenarios green in
all variants", and quoting one invites the wrong conclusion.

| variant | green | unresolved | failing |
|---|---|---|---|
| `dev` | 38 | S01, S04, S07, S23, S29 | S39 |
| `ci` | 36 | S07, S29 | **S21** |
| `full` | S01, S02, S04 | — | **S03, S05** |

**`dev`.** S39's failure is a run-parameter error, not a defect: the card refuses `--duration 300`
because it needs at least 500 s, and says so on entry with the number. S01, S04 and S29 have no
premise to test at that size and are green at larger ones.

**`ci`.** The only complete post-fix pass. S11 and S41 failed here before the fixes below and are
re-verified green.

**`full`.** Never run to completion — interrupted at scenario 5 of 44 after two hours. Both failures
are new and appear only at this scale.

## Fixed and verified {#fixed-and-verified}

| card | what was wrong | evidence |
|---|---|---|
| S06 | verdict gated on `CASBlobGet`, which column-data reads never increment | PASS 10/10, was inconclusive at all three scales |
| S15 | `formatDateTime(now(),'…%H:%M:%S')` — `%M` is the MONTH NAME in ClickHouse, so every scoped query threw `Code: 41` | PASS 11/11, was 5 of 8 verdicts unresolved |
| S20 | counter window opened after `wait_healthy`, but the fetch and relink complete in 20 ms inside it | PASS 12/12, 37 counters moving where the delta had been empty |
| S21 (`ci`) | four stacked defects: inert counter, 1.97 TB `full`, `readers: 16` saturating the store, merges collapsing the 100 parts that ARE the premise | PASS 13/13, verdict separates subset from full scan 59-fold |
| S27 | LIST anomalies armed on `cas/refs/`, a prefix absent from the layout | retargeted to `/cas/ns/stream/`, PASS in the `s3faultproxy` variant |
| S10 | `lw_supported = False` hardcoded made the lightweight `DELETE` beneath it dead code | patch parts observed for the first time: 3 at `ci`, 4 at `full` |
| S11 | GC-round ceiling hardcoded at 30 s while work grows as `parts x rows` | ceiling now scales; PASS at `dev` and `ci` |
| S41 | variant endpoints never applied; three sites binding a stale container name at import | 0 verdicts to 45/45 |
| S01/S02 | `full` demanded 100 GiB, needing 550+ GiB of disk | 8 GiB keeps 64x headroom over the verdict's noise floor; PASS |
| harness | `predown_dump` timeout escaped and killed the scenario it serves, before the reset that would have healed the cluster | verified in the wild: the timeout now logs and the run continues |
| instrumentation | `Memory` traces never dumped; `total_memory_profiler_step` defaulted to 0 | 1,064 background samples where the count would have been zero |
| product | `EXPORT PARTITION` refused on a CAS disk | Altinity#2291, verified end to end with a data oracle |

### Two defect classes worth remembering {#defect-classes}

**A verdict gated on a counter its own path does not increment.** Found three times (S06, S21, S20).
A sweep then checked every verdict gating on a `CAS*` counter — 52 counters, 289 references, 52 gating
uses. No counter is a ghost; all exist with real increment sites. The class is narrow and now closed:
only the 14 backend-table counters can be inert, and only on the one path that bypasses
`CasInstrumentedBackend`.

**A value bound at import, not at use.** Three of day two's four defects were one bug in three files
(`lifecycle`, `observe`, `checkpoint`): an environment variable read at import and then used as a
DEFAULT ARGUMENT, which binds it a second time at definition. A variant needing different endpoints
could never be honoured however late the variable was set. The search that finds this is for where a
value is BOUND, not where the environment is read.

## Filed in the backlog {#filed-in-backlog}

| entry | substance |
|---|---|
| `[mount-renewal-loses-the-lease-on-one-unresolved-attempt]` | S03 at `full`: `attempts_sent = 1`, one heartbeat write hung 23.7 s against a 30 s TTL, lease surrendered with no retry. Separately, one node stopped with 1,969 ms of confirmed budget left and `stop_cause = continue` |
| `[s05-standalone-repoints-on-the-non-transactional-path]` | 1,200 standalone repoints of committed refs during sparse-write GC, `full` only, 16 verdicts and zero anomalies |
| `[s01-rss-growth-scales-with-the-blob]` | RSS growth 0 at a 512 MiB blob, 2.228 GiB (28%) at 8 GiB; trace puts 42% in String deserialization under the merge and 7% in the CAS write path |
| `[soak-retry-budget-turns-a-503-into-a-livelock]` | a 500-attempt retry budget converts RustFS's read-concurrency refusal into an unbounded wait |
| `[ca-event-log-loses-gc-manifest-deletes]` | GC deleted 517 manifests; the event log recorded one, though emission is unconditional per attempt |
| `[s23-idle-baseline-measures-the-telemetry]` | an idle pool's background allocator is system-log flushing, ~176 MB/hour, with no CAS frame in the top |

## Open {#open}

**S21 — the only failure with no explanation.** Hangs 4 of 4 at `ci`, passes at `dev`. RustFS reports
`permits_in_use: 256/256` at 100% queue utilization, answering `503` after ~5 s while its CPU sits at
0.13%; a query thread and the GC thread both wait in `poll` on live connections, 545 `ESTABLISHED` on
both sides. `s3_max_connections = 192` cut throttling from 34% to 6.8% and did not fix the hang, so it
is committed as a measured improvement and explicitly not as the fix.

**S07 and S29** — both need a limit lowered rather than a workload raised, and neither has been done.
S07's own note says a cap-lowering test setting is required (`kMaxManifestEntries = 1048576` is
unreachable through SQL). S29's advice to "rerun at ci/full" is stale: the `full` run it recommends
still leaves `non_blob_pool_bytes` at 1.7 MB, too small to attribute RSS.

**S23** — two verdicts blocked by "compose fixed at 2 servers" though a 10-replica compose exists, and
a 64 MiB memory threshold that at this profiling configuration mostly measures the server's own
telemetry.

**The `full` variant has never completed.** Five of 44 in two hours; at that rate ~15 hours. Running
it whole is probably the wrong shape — cards already green at `dev` and `ci` mostly re-prove
themselves there, while S03, S05, S07, S21, S29 and the heavy adversarial cards are where `full` earns
its cost.

## To research {#to-research}

Three questions that need a measurement, not an argument.

**Why the mount-lease heartbeat never retries.** One unresolved write surrenders the lease and fences
the mount, on a disk whose ordinary reads retry up to `s3_retry_attempts = 500`. And why one node
stopped with ~2 s of proven safety margin unspent.

**Which operation calls `repointRef` outside a transaction.** 1,200 of them in S05. Check whether the
count scales with tables, parts or GC rounds; if it shares a call site with the known
`delete_tmp_*` repoint waste (~22% of the writer PUT class), these are one finding.

**Where 516 `manifest_delete` events go.** Emission is unconditional per attempt in `CasGc.cpp`
PHASE 15/18, yet 517 deletions produced one event. Until this is answered, manifest reclaim cannot be
audited from `cas_log`.

## Method notes {#method-notes}

Recorded because they cost more time than any single defect.

**Guess-and-check masquerading as empiricism.** Runs were cheap to launch, so plausible explanations
got acted on and then refuted by the run. Five separate explanations for S21's hang were each
disproved by measurement: store overload, a retry storm, a missing rate limit, a growing `CLOSE_WAIT`
leak, and the suite context. The tell is reading evidence after acting rather than before.

**The S20 case is the one to keep.** Two plausible hypotheses — wrong counter, asynchronous
publication — both false, and the first would have produced a GREEN verdict, because the counter it
proposed does move. Acting on it would have masked a window defect while looking like success. What
settled it was evidence that DISTINGUISHES hypotheses rather than merely fitting one: part names and
microsecond timestamps.

**Write a setting only after finding where it is declared.** Two no-op edits in one session: a server
setting placed in `users.d`, and a profile setting placed in a disk block across 24 files.
`Settings.cpp` means a profile, `ServerSettings.cpp` means `config.d`, `S3RequestSettings.cpp` means
the disk block, `MergeTreeSettings.cpp` means the table.

**A cascade can fake four failures.** S21 saturated the store, `predown_dump` then exceeded its
timeout and threw before the reset that would have healed the cluster, and S22, S23 and S25 each died
on entry with zero verdicts, ten minutes apart. Only the first failure was real. The same reading
should be applied to any suite report before its failures are counted as independent.
