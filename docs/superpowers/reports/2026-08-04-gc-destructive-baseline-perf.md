---
description: 'Phase-level cost decomposition of a real destructive GC round on the preserved
  Stage-B specimen, re-derived directly from the archived TSV dumps; ranks next-round
  optimization opportunities (MultiDelete, delete concurrency) against measured ceilings.'
sidebar_label: 'GC destructive-baseline perf'
sidebar_position: 1
slug: /development/cas/gc-destructive-baseline-perf
title: 'GC destructive-baseline performance research'
doc_type: report
---

# GC destructive-baseline performance research {#gc-destructive-baseline-perf}

## 1. Scope and evidence rule {#scope}

This report is Stage-B Task T9, the mandatory closeout for the destructive-GC work. It measures
the ONE specimen named in the dispatch — never re-runs the soak, per the rule that a second run is
a different specimen and mixing the two silently is the exact failure this report exists to
prevent.

**Specimen:** `utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/general_soak_90m_run3_seed20260808_specimen/`
— the 90-minute general soak (d), seed `20260808`, the "sequential-baseline destructive workload"
(no `MultiDelete`, no delete-side concurrency — see the plan's `{#t9}`), also the specimen behind
T8's `{#step-3c-cost-inventory}`. All figures below come from `predown_ch1/gc_log.tsv` (7.4M-row
`cas_log.tsv` was NOT needed for this report — the phase-level `ProfileEvents`/`phase_metrics`
maps already embedded in `gc_log.tsv` carry every S3-verb count this report uses) and
`predown_ch2/gc_log.tsv`, queried with `clickhouse-local` against a schema typed from the files'
own `TabSeparatedWithNames` header (`ProfileEvents Map(String, Int64)`, `phase_metrics
Map(String, Int64)` — ClickHouse's own TSV map serialization, so the columns parse directly; every
query is restated in `{#evidence-index}`).

**What these numbers CAN answer:** where the wall time and the S3-request count of a real,
destroying GC round go, phase by phase, on this backend (RustFS, S3-compatible, low per-request
latency), and whether `MultiDelete`/delete concurrency have a measured ceiling worth chasing.

**What they CANNOT answer:** wall-time behavior on real S3 (this backend's ~650–700µs/request mean
delete latency, derived below, is not representative of AWS S3's typical 10s of milliseconds —
every wall-time ceiling in this report is a REQUEST-COUNT ceiling first and a wall-time number
second, and the note says so at each use); anything about a workload that isn't this one (a quieter
pool, a different fold-edge density) — this is one destructive round shape, not a general
performance model.

**Predecessor:** `docs/superpowers/reports/2026-07-29-gc-perf-audit-soak.md`
(`{#opportunities}`). Its verdicts against this baseline are in `{#predecessor-verdicts}`.

### A correction found while re-deriving: T8's Step-3c inventory does not reproduce from this archive {#specimen-reconciliation}

Step 3 of the plan requires re-deriving every figure from the artifacts immediately before
committing. Doing that surfaced three concrete mismatches between T8's published
`{#step-3c-cost-inventory}` table and what is actually in the preserved specimen archive — not
close-enough rounding, but numbers this archive cannot produce by any grouping this report tried.
They resolve to **two distinct mechanisms, confirmed independently** (cross-checked against a
second, independent re-derivation by the team lead) rather than one:

**Mechanism A — `ch2`'s live query ran later than the predown snapshot.** T8 states
`pending_deletes` ran 108 times (ch1 63, ch2 45). Direct counts against `predown_ch2/gc_log.tsv`
(`awk -F'\t' '$27=="pending_deletes"'`, cross-checked with a `clickhouse-local` `GROUP BY phase`)
give **38**, not 45, and the same `ch2`-only gap recurs for every phase in the table
(`manifest_deletes` 38 not 45, `ref_object_cleanup` 38 not 45, `namespace_cleanup` 76 not 102).
T8's criterion-3 evidence separately cites `ch2` rounds 105–108 explicitly (condemned 168 →
graduated 168 → redeleted 168 → two further zero rounds); `predown_ch2/gc_log.tsv`'s highest
`round` value is **101** (`event_time` 06:29:45, `awk -F'\t' 'NR>1{print $10}' | sort -n |
tail -1`) — rounds 105–108 are not present in the archive at all. Both point the same direction:
T8's `ch2` figures reflect MORE `ch2` activity than the archived predown snapshot contains,
consistent with T8's live query having run later, against the still-up cluster, than the moment
`predown_ch2/gc_log.tsv` was captured (the specimen's own `README.md` states the predown dump is
taken "the instant the run returned, before any teardown" — apparently earlier than whatever
wall-clock point T8's live pass queried).

**Mechanism B — `pruned_through` was reported as a SUM, on `ch1` too, contrary to its own stated
caveat.** T8 reports "reached 1830 (ch1) / 3735 (ch2) by run end," explicitly flagged there as "a
per-node high-water mark, not summable." The archive's actual maximum `pruned_through` value on
any `round_commit` row is **60** (ch1, 63 rows) / **98** (ch2, 38 rows)
(`awk -F'\t' '$27=="round_commit"{print $29}' | grep -o "'pruned_through':[0-9]*" | sort -t: -k2
-n | tail -1`). A `sumMap()` over those same rows instead gives **exactly 1830 (ch1)** and 3021
(ch2) — `ch1`'s figure in T8 is precisely the sum, not close to it, despite T8's own sentence
saying not to take the sum. This is a genuinely different error than Mechanism A: it reproduces on
`ch1`, where every invocation count otherwise matches T8 exactly — so **`ch1` does NOT match T8
"exactly everywhere"**, and this report's earlier framing to that effect was itself wrong and is
withdrawn here. `ch2`'s `pruned_through` (3735 in T8) is higher than even `ch2`'s own sum (3021),
so `ch2` carries both mechanisms at once.

**None of this changes T8's six-criteria PASS verdict** — criterion 1's family-level "every family
did real work" claim holds on the archived range too (every family still shows nonzero destructive
work on the corrected counts), and criterion 3's zero-backlog claim holds on the archive's own
evidence (`ch2`'s last-Success-round evidence at round 100, condemned 0 graduated 769 redeleted
769, precedes zero-work rounds inside the archive itself, corroborated independently by the fsck
checkpoint trend in criterion 2). But the specific cited numbers (108/45, rounds 105–108, 1830/3735)
are not reproducible from the artifact both this report and T8 were told is authoritative, and are
corrected — in both documents — rather than repeated. `2026-08-03-stage-b-RESULTS.md` has been
corrected in its criterion-1 row, criterion-3 row, and the Step-3c inventory table + its
introductory note, per the team lead's confirmation and explicit direction to proceed.

Every number in the rest of this report is the archive's own, re-derived independently.

## 2. Phase decomposition of the destructive round {#phase-decomposition}

`ch1` held leadership through round 63 (04:53:05–06:17:06 UTC), then lost it during the chaos-induced
mount-fence trip; `ch2` picked up leadership through round 101 within the archived window
(04:53:11–06:30:26 UTC). All wall times are `sum(phase_duration_microseconds)` per phase; `n` is the
row count.

| Phase | ch1 n | ch1 wall (s) | ch1 max (s) | ch2 n | ch2 wall (s) | ch2 max (s) |
|---|---|---|---|---|---|---|
| `fold_ref_intake` | 63 | 2303.00 | 1659.93 | 38 | 0.90 | 0.21 |
| `defer_decision` | 69 | 722.65 | 77.66 | 76 | 2.18 | 0.08 |
| `manifest_deletes` | 63 | 409.52 | 184.35 | 38 | 0.22 | 0.12 |
| `ref_object_cleanup` | 63 | 267.30 | 56.04 | 38 | 0.46 | 0.38 |
| `lease` | 118 | 234.42 | 209.28 | 553 | 244.75 | 210.94 |
| `pending_deletes` | 63 | 208.77 | 15.99 | 38 | 0.42 | 0.38 |
| `fold_reduce` | 63 | 191.27 | 98.76 | 38 | 15.68 | 0.90 |
| `namespace_cleanup` | 69 | 13.93 | 1.12 | 76 | 2.08 | 0.10 |
| `round_commit` | 63 | 0.355 | 0.023 | 38 | 0.148 | 0.013 |
| `heartbeat_floor` | 69 | 0.347 | 0.032 | 76 | 0.230 | 0.006 |
| `pre_fold_ref_drain` | 69 | 0.265 | 0.049 | 76 | 0.149 | 0.006 |
| `fold_ref_group` | 63 | 0.117 | 0.010 | 38 | 0.001 | 0.0001 |
| `fold_seal_read` | 63 | 0.093 | 0.007 | 38 | 0.042 | 0.003 |
| `fold_seal_write` | 63 | 0.083 | 0.006 | 38 | 0.028 | 0.001 |
| `parent_seal_read` | 63 | 0.052 | 0.005 | 38 | 0.024 | 0.003 |
| `meta_pool_wait` | 63 | 0.0012 | 0.0010 | 38 | 0.0008 | 0.0008 |
| `orphan_sweep` | 63 | 0.0005 | 0.0003 | 38 | 0.00007 | 0.000003 |
| `handoff_reclaim` | 63 | 0.0003 | 0.0002 | 38 | 0.00004 | 0.000007 |
| **sum, named phases** | | **4352.1** | | | **267.3** | |

`ch1`'s summed phase wall (4352.1s) against its 5803s archived span is **75.0% GC-phase-busy**.

### S3 operations by verb — the four destructive families, T8's Step-3c lines re-derived {#step-3c-verbs}

Combined `ch1+ch2`, re-summed from the same archive (not the disputed 108/45 counts above — these
are the request totals, which is what T8's cost inventory was really trying to convey):

| Phase | `DiskS3DeleteObjects` | `DiskS3GetObject` | `DiskS3HeadObject` | wall (s) |
|---|---|---|---|---|
| `pending_deletes` | 281,754 | — | — | 209.19 |
| `manifest_deletes` | 522,308 | — | — | 409.74 |
| `ref_object_cleanup` | 139,838 | 280,054 | 419,892 | 267.76 |
| `round_commit` (generation pruning) | 255 | — | — | 0.503 |
| **total, 4 destructive families** | **944,155** | **280,054** | **419,892** | **887.2** |

`944,155` is also exactly `281,754+522,308+139,838+255` and exactly the total `DiskS3DeleteObjects`
summed over every named-phase row in the whole archive (`{#evidence-index}` query 9) — the four
destructive families are the ONLY source of deletes in this round; nothing else in the phase table
deletes an object.

### Whole-round S3 verb totals (all named phases, both nodes) {#whole-round-verbs}

| Verb | Count |
|---|---|
| `DiskS3DeleteObjects` | 944,155 |
| `DiskS3GetObject` | 1,536,268 |
| `DiskS3HeadObject` | 1,984,556 |
| `DiskS3CreateMultipartUpload` | 24 |
| `DiskS3ReadRequestsCount` | 3,520,851 |
| `DiskS3WriteRequestsCount` | 944,179 |

`fold_ref_intake` alone accounts for 1,183,381 of the `GetObject` total and 1,183,381 of the
`HeadObject` total (a 1:1 pairing — see `{#untimed-and-adjacent}`), and is by far the single
largest wall-time phase (2303.0s of ch1's 4352.1s, 52.9%).

### Time and rounds to fixpoint {#fixpoint}

`ch1`'s backlog reaches zero within the archive: round 61 graduated/redeleted 5000, round 62
graduated 0 (958 carried), round 63 graduated=redeleted=deleted=0 at 06:17:06 — then ch1 loses
leadership (a run of `NotALeader` Finish rows through the archive's end at 06:29:48), so ch1's own
tail cannot independently confirm a ≥3-round stable-zero bar; leadership moved to ch2 instead.

`ch2`'s tail inside the archive: round 100 Success at 06:28:04 (graduated=redeleted=769, real
work), then round 101 (one Deferred, one Success, one Deferred, all `condemned=graduated=redeleted=0`)
at 06:29:25–06:29:45. That is 2 further zero-round observations after the last nonzero round —
consistent with reaching fixpoint, but short of the ≥3-round bar T8's criterion 3 states, because
(per `{#specimen-reconciliation}`) the rounds T8 cites to close that bar (105–108) are not in this
archive. **Time to fixpoint, honestly bounded by what the archive contains:** the backlog is
already at zero by ch2 round 101 / 06:29:45, i.e. no more than **5,794s** (96.6 min) after the
observed run start (04:53:11) — this is an upper bound, not the true fixpoint time, since the
archive's own last activity is a Deferred/Success/Deferred cluster still trailing off, not a proven
stable plateau.

## 3. Un-timed spans, named {#untimed-and-adjacent}

- **`orphan_sweep`**: empty `ProfileEvents` map on every row (both nodes) despite real logical
  activity (`phase_metrics`: ch1 `listed=5501 skipped=5501 list_budget_keys=63000`). The LIST/scan
  work this phase logically performs carries no S3-verb attribution on the row itself.
  **What would answer it:** thread the `ProfileEvents` snapshot from wherever the actual LIST calls
  execute (if off the round thread, a worker-pool context) back into this row, or split it into a
  separate phase row scoped to that worker.
- **`meta_pool_wait`**: same pattern — empty `ProfileEvents` (ch1: `{}`) despite
  `phase_metrics: jobs_completed=569,703 jobs_scheduled=569,703` — over half a million real
  freshness-meta jobs whose S3 cost is invisible on this row because they execute on the
  `meta_pool` worker threads, not the round thread this row is timed on. **What would answer it:**
  the same fix as `orphan_sweep` — attribute `ProfileEvents` at the worker-pool boundary, not only
  at the round-thread phase boundary.
- **`defer_decision`** is NOT un-timed (722.65s wall, ch1) but is a genuine surprise: only 402 S3
  requests (`DiskS3ReadRequestsCount`) accompany that wall time, against `phase_metrics:
  ref_keys_listed=1,771,864 ref_log_keys_listed=1,761,341`. The time is not S3-bound — it is
  CPU/lock-bound processing of ~1.77M already-known ref keys on the round thread, one
  `for`-loop, per round (source: `Gc/CasGc.cpp:2134`, cited by the existing BACKLOG entry
  `[gc-frontier-one-list] {#gc-frontier-one-list}`). This report adds a fresh, concrete number to
  that entry (722.65s / 69 rounds ≈ 10.5s/round of pure-CPU decision time on ch1 alone) rather than
  opening a new one.

No estimate is presented as a measurement anywhere above — every un-timed span is reported as
`{}` / zero S3-verb attribution with the real logical-work counters shown alongside it, not
guessed at.

## 4. Before/after: only where honest {#before-after}

### Probe A removal (T5) — a real before/after {#probe-a-before-after}

Probe A (`ref_list_probe`, `gc_probe_a_period`) is gone from this specimen entirely — zero rows in
either node's `gc_log.tsv` with `phase='ref_list_probe'`. It existed and ran in the earlier
preserved T15 re-validation specimen (`utils/ca-soak/logs/predown/ch1/soak_t15_revalidation/gc_log.tsv`,
pre-`T5`, same TSV schema), so that is the honest "before":

| | Before (`T15` specimen, pre-`T5`) | After (this specimen, post-`T5`) |
|---|---|---|
| Rows / round coverage | 64 (`ref_list_probe` ran every round) | 0 (phase does not exist) |
| Wall time | 326.156s (ch1, 64 rounds) | 0s |
| `CasRootList` (LIST-page ops) | 709 | 0 |
| Keys listed (`keys_listed`) | 707,805 | 0 |
| Sampled-due firings | `CasGcProbeADue`=4, `Performed`=4 (1-in-16 of 64) | n/a — no detector left to fire |

This is a real before/after because both specimens measure the SAME phase's absence, not two
different workloads' totals — every round pre-`T5` paid the enumeration cost (`CasRootList`/
`keys_listed`) regardless of whether the 1-in-16 sampled check was "due" that round; every round
post-`T5` pays nothing. T5's own deletion is confirmed delivered.

### Destruction (T6) — NOT compared to a "before" {#destruction-no-before}

Destruction never ran as a full round before T6 landed — every earlier soak either had rounds that
never terminated (the T14 pre-Task-15 specimen) or ran with `suppress_destructive=1` (dry-run,
every delete family inert by construction). There is no prior specimen where `pending_deletes`,
`manifest_deletes`, `ref_object_cleanup`, and generation pruning did real work, so this report does
not compare this round's cost against one — that would be comparing against a suppressed round
that measured a fundamentally different (zero-delete) workload. This section states that plainly
instead of manufacturing a before/after.

## 5. Ranked opportunities for the next round {#opportunities}

Each entry names the phase(s) it touches, the measurement that motivates it, and a condition that
would falsify ranking it here.

### 1. `MultiDelete` is blocked on the conditional-delete gap, not on wiring {#opp-multidelete}

**Measurement.** All 944,155 `DiskS3DeleteObjects` calls across the four destructive families are
single-key requests — `Backend::deleteExact` (`Backend/CasObjectStorageBackend.cpp:955`) calls
`object_storage->removeObjectIfTokenMatches`, which issues one `DeleteObjectRequest` per call
(`S3ObjectStorage.cpp:487-496`) with an `If-Match` ETag precondition. Every delete-family call site
in `Gc/CasGc.cpp` — including `deletePrefixWholesale`, which already LISTs a whole prefix in
batches — still calls `deleteExact` once per listed key (`Gc/CasGc.cpp:3563-3570`). ClickHouse
already has a working batch-delete path (`deleteFilesFromS3`, `IO/S3/deleteFileFromS3.cpp:80`,
default batch size 1000 — `IO/S3Defines.h:48`), reachable via `S3ObjectStorage::removeObjectsImpl`
— but it is never called from CAS's delete-family phases, and by construction it CANNOT be: the
batch `DeleteObjects` request only sets `Key` on each `Aws::S3::Model::ObjectIdentifier`
(`deleteFileFromS3.cpp:118-122`) — no per-key conditional precondition exists in that AWS API, so
switching to it as-is means giving up the exact-token match that is the CAS resurrection-safety
invariant (never delete a token a writer may have already displaced).

**Ceiling.** If the conditional-delete constraint were resolved (or judged safe to drop for a
provably-immutable cohort), the REQUEST-COUNT ceiling is `944,155 → ⌈944,155/1000⌉ = 945` batch
calls — a >99.9% reduction in delete request count. This is the honest ceiling to report: it is a
request-count number, not a wall-time prediction, because this specimen's backend (RustFS) shows
~650–700µs mean per-delete latency (`DiskS3WriteMicroseconds`/`DiskS3DeleteObjects` = 181.3s/280,958
≈ 645µs for `pending_deletes` alone) — far below typical real-S3 RTT, so this specimen cannot
honestly measure what wall-time win `MultiDelete` would produce against AWS S3; only the
request-count reduction transfers directly.

**Falsification.** REFUTED as a near-term win if no design closes the per-key conditional gap
(batching an ordinary `DeleteObjects` over currently-conditional keys is a correctness regression,
not an optimization, and this report does not recommend it as stated). Worth ranking #1 anyway
because it is the largest verb-count reduction available and the predecessor's audit never reached
it; the blocking question — can a cohort of keys be proven collision-free at round-commit time
without a per-key conditional check — is itself the next research step, not a code change.

### 2. Delete-side concurrency is close to fully serial today {#opp-delete-concurrency}

**Measurement.** For `pending_deletes` (ch1): wall 208.77s vs. summed per-request
`DiskS3WriteMicroseconds` 181.3s — 87% of wall time is accounted for by the sum of individual
request latencies, i.e. requests are overlapping very little. `manifest_deletes` shows the same
shape (wall 409.52s vs. summed 368.56s, 90%). Both phases already dispatch through
`LocalThreadPoolJobs` (one job roughly per delete), so the thread-pool machinery exists; the
workload is simply run with effectively no concurrent in-flight deletes, exactly matching the
plan's explicit "sequential-baseline... no delete-side concurrency" framing for this soak.

**Ceiling.** A bounded worker pool issuing K concurrent conditional deletes (same shape as the
existing `meta_pool`) could plausibly cut wall time toward `wall/K` for `pending_deletes` and
`manifest_deletes` specifically — together 618.29s of ch1's 4352.1s phase-wall total (14.2%).

**Falsification.** REFUTED if concurrent deletes against the same prefix trigger backend
throttling (RustFS or S3 `SlowDown`/503) at a K this specimen never tried — this baseline issued
deletes near-serially and cannot rule that out. Ranked below `MultiDelete` because it is
orthogonal and smaller (14.2% of ch1's own phase wall vs. request-count reduction touching 100% of
delete traffic), and because the two levers compose (concurrent batch calls) rather than compete.

### 3. `fold_ref_intake`'s 1:1 HEAD/GET pairing is a different HEAD than the one Task 15 already removed {#opp-fold-head-successor}

**Measurement.** `fold_ref_intake` (the single largest wall-time phase, 2303.0s / 52.9% of ch1's
phase-wall total) shows `DiskS3GetObject`=`DiskS3HeadObject`=1,183,381, an exact 1:1 pairing. The
predecessor's `{#opp-fold-head}` (drop the HEAD in `foldManifestEdges`) is CONFIRMED delivered —
`Gc/CasGc.cpp:1301-1312`'s own comment states the HEAD was removed because "the GET alone carries
the absence signal a HEAD would have carried." The HEAD still visible in this specimen's
`ProfileEvents` is a DIFFERENT one: `ReadBufferFromS3::getObjectSizeFromS3` (`IO/ReadBufferFromS3.cpp:463-469`)
issues a `HeadObject` to learn `Content-Length` before every ranged `GetObject`, generically, for
every S3 disk read in ClickHouse — not CAS-specific, and not the site opportunity 3 targeted.

**Ceiling.** Unmeasured here — this report only establishes that the pairing exists and traces to
a generic read-path call, not to CAS logic. Sizing the win (and checking whether an existing
known-size read-buffer constructor already avoids this HEAD on some call paths) is future work, not
concluded here.

**Falsification.** REFUTED as a CAS-scoped fix if the size-probe HEAD turns out to be required by
every generic S3 disk consumer for correctness (e.g. detecting truncated/resized objects
mid-read) — in that case this is a ClickHouse-wide question, not a CAS one, and does not belong on
this BACKLOG at all. Ranked third (evidenced but unsized) rather than first.

## 6. Predecessor opportunities: confirmed / refuted / untouched {#predecessor-verdicts}

1. **`{#opp-bounded-rounds}` (make rounds terminate).** CONFIRMED, and holding under a real
   destructive workload: this specimen shows bounded rounds throughout (ch1 63 Success rounds
   across 5803s, ch2 continuing past round 100 with real Finish rows, none unbounded).
2. **`{#opp-relink-noise}` (stop paying ERROR-severity exceptions for relink refusals).** LEFT
   UNTOUCHED by this report — this specimen's `errors.tsv` shows `NETWORK_ERROR` at 7 (ch1) / one
   row per code, both nodes, nothing resembling the predecessor's 94,999/123,907-refusal storm.
   Cannot be scored confirmed/refuted from a workload that never reproduces the symptom; needs a
   specimen shaped like the predecessor's busy-relink chaos window.
3. **`{#opp-fold-head}` (drop the HEAD in `foldManifestEdges`).** CONFIRMED delivered at the
   CAS-logic level (source comment, `Gc/CasGc.cpp:1301-1312`) — see `{#opp-fold-head-successor}`
   above for the DIFFERENT, generic-read-path HEAD this report newly found sitting on the same
   phase.
4. **`{#opp-ca-log-restart}` (stop the CA audit log from dominating restart).** LEFT UNTOUCHED —
   out of scope for a destructive-round cost inventory (a startup-path finding, not a GC-round
   one); this report's artifacts do not include a restart.
5. **`{#opp-follower-poll}` (back off the follower's lease poll).** LEFT UNTOUCHED as ranked, but
   this specimen's `lease` phase numbers are worth recording for whoever picks it up: ch2 (mostly
   follower) spent 244.75s across 553 `lease` rows (0.44s avg) — small per-poll, matching the
   predecessor's own characterization, still USER-GATED as lease-protocol timing.

## 7. BACKLOG entries {#backlog-entries}

One entry per ranked opportunity added to `docs/superpowers/cas/BACKLOG.md`:
`[gc-multidelete-conditional-gap]`, `[gc-delete-concurrency-serial]`,
`[gc-fold-intake-readbuffer-head]`.

## 8. Evidence index {#evidence-index}

All paths relative to the repository root. `SCHEMA` below is the file at
`build/t9_scratch/gc_log_schema_specimen.sql` (29 columns, typed from the specimen's own TSV
header; `ProfileEvents`/`phase_metrics` as `Map(String, Int64)` — ClickHouse's own TSV map
serialization, so `clickhouse-local` parses them directly against `file(..., TabSeparatedWithNames,
'$SCHEMA')`).

| # | Figure(s) | Artifact | Query / command |
|---|---|---|---|
| 1 | Phase decomposition table (`{#phase-decomposition}`) | `predown_ch{1,2}/gc_log.tsv` | `clickhouse-local --query "SELECT hostnameTag, phase, count(), sum(phase_duration_microseconds)/1e6, max(...)/1e6, avg(...)/1e6 FROM (SELECT *, 'ch1' AS hostnameTag FROM file(ch1 path, TabSeparatedWithNames, SCHEMA) UNION ALL SELECT *, 'ch2' FROM file(ch2 path, ...)) WHERE phase != '' GROUP BY hostnameTag, phase ORDER BY hostnameTag, wall_s DESC"` |
| 2 | Step-3c verb re-derivation table (`{#step-3c-verbs}`) | same | `SELECT sumMap(ProfileEvents), sumMap(phase_metrics) FROM file(ch{1,2} path, ...) WHERE phase='<pending_deletes|manifest_deletes|ref_object_cleanup|round_commit>'` per node, summed by hand |
| 3 | Whole-round verb totals (`{#whole-round-verbs}`) | same | `SELECT sumMap(ProfileEvents) FROM (SELECT * FROM file(ch1,...) UNION ALL SELECT * FROM file(ch2,...)) WHERE phase != ''` |
| 4 | `fold_ref_intake` HEAD/GET pairing | `predown_ch1/gc_log.tsv` | `SELECT sumMap(ProfileEvents), sumMap(phase_metrics) FROM file(ch1, ...) WHERE phase='fold_ref_intake'` |
| 5 | `defer_decision` S3-op-vs-wall mismatch | `predown_ch1/gc_log.tsv` | same query, `phase='defer_decision'` |
| 6 | Un-timed `orphan_sweep`/`meta_pool_wait` (`{#untimed-and-adjacent}`) | `predown_ch1/gc_log.tsv` | same query, `phase='orphan_sweep'` / `'meta_pool_wait'` |
| 7 | Probe-A before (`{#probe-a-before-after}`) | `utils/ca-soak/logs/predown/ch1/soak_t15_revalidation/gc_log.tsv` | `SELECT sumMap(ProfileEvents), sumMap(phase_metrics), count(), sum(phase_duration_us)/1e6, max(...)/1e6 FROM file(..., TabSeparatedWithNames, T15_SCHEMA) WHERE phase='ref_list_probe'` (note: this file's own header names the duration column `phase_duration_us`, not `_microseconds` — the T9 schema was adjusted per-file, both confirmed against their own headers before use) |
| 8 | Probe-A after (absence) | `predown_ch{1,2}/gc_log.tsv` | `SELECT phase, count() FROM file(...) WHERE phase != '' GROUP BY phase ORDER BY phase` — `ref_list_probe` absent from both |
| 9 | `944,155` total `DiskS3DeleteObjects` cross-check | `predown_ch{1,2}/gc_log.tsv` | query 3's `pe['DiskS3DeleteObjects']`, cross-checked against the query-2 per-family sum (both give 944,155) |
| 10 | Fixpoint tail, ch1/ch2 (`{#fixpoint}`) | `predown_ch{1,2}/gc_log.tsv` | `awk -F'\t' '$5=="Finish"{print $10,$11,$18,$19,$20,$13,$3}'` tail, both files |
| 11 | Specimen-reconciliation counts (`{#specimen-reconciliation}`) | `predown_ch2/gc_log.tsv` | `tail -n +2 <file> | awk -F'\t' '{c[$27]++} END{for(k in c) print k, c[k]}'`, and `$5=="Finish"{c[$11]++}` for the outcome split; `awk -F'\t' 'NR>1{print $10}' | sort -n | tail -1` for max round; `awk -F'\t' '$27=="round_commit"{print $29}' | grep -o "'pruned_through':[0-9]*" | sort -t: -k2 -n | tail -1` for the high-water mark |
| 12 | `Backend::deleteExact` single-key call sites | `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:955`, `Gc/CasGc.cpp:3563-3570` | direct read |
| 13 | `deleteFilesFromS3` batch path exists, unused by CAS | `src/IO/S3/deleteFileFromS3.cpp:80-140`, `src/IO/S3Defines.h:48` | direct read |
| 14 | `ReadBufferFromS3` size-probe HEAD | `src/IO/ReadBufferFromS3.cpp:463-469` | direct read |
| 15 | `foldManifestEdges` HEAD already removed | `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1301-1312` | direct read |
| 16 | Whole-round GC-busy fraction (75.0%, ch1) | derived from query 1's ch1 sum (4352.1s) over the archive's own ch1 span | `awk -F'\t' 'NR>1{print $3}' predown_ch1/gc_log.tsv | sort | sed -n '1p;$p'` for the span (04:53:05–06:29:48 = 5803s) |

**Figures NOT included because they could not be re-derived from the archive:** T8's `pending_deletes`
ch2 invocation count (45), `namespace_cleanup` totals of 171/108, rounds 105–108, and the `pruned_through`
1830/3735 figures — all deleted rather than repeated, per `{#specimen-reconciliation}`.
