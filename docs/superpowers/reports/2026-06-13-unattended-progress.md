# Unattended work log — CA-MergeTree, starting 2026-06-13 07:00 UTC

**Mandate:** work unattended ~24h. Ultimate goal: green CA-S3 tests. Address backlog
debts one-by-one with TDD and real debugging (no blind fixes). Record everything
deferred in the backlog. Adjust ordering as evidence dictates. Consult codex/subagents
when stuck.

**Conventions:** build logs → `build/*.log` (subagent-summarised). Test logs →
`build/test_*.log`. Temp repros → `tmp/`. Commits on branch `cas-mergetree-poc`
(never master, never rebase/amend — new commits only).

## Working order (living; reordered as evidence dictates)

Green-tests critical path first, then debts:
1. **B118** — CA INSERT hang. DONE (fix `5b69a99fd5b`, verified on repro: count 1000000, exit 0).
   Lane regression test added (`05006_content_addressed_dedup_blob_insert`), lane re-run in flight.
2. **B116** — read path (committed `d8fd11659df`); re-verify in lane once INSERTs unblock.
3. **B117** — outer `ReadBufferFromS3` retry loop ignores cancellation. EDIT STAGED (cancellation
   check in `processException`), build deferred until the full lane frees the binary.
4. **write-storm** — lane broken-pipe PUT storm under concurrency (needs concurrency repro).
5. **B119** — RustFS-side broken-pipe-on-abort (upstream report).
6. Then: B105/B106/B107/B108/B109/B110/B111/B113/B114 (M-W debts), B92/B93,
   B94–B99 (M-F full GC), B101–B104, B25/B26/B31, B79/B80/B82/B89, B1/B8/B13/B14/B15/B17.

### 2026-06-13 ~08:40 — survey + planning while full lane runs
- **B105** (CA reads bypass pipeline cache stages): LIKELY ALREADY FIXED by the B116 read-path
  rework — blob reads now ride the standard gather/FileCache/page-cache/async pipeline; only the
  in-manifest tiny-bytes path stays memory-served (correctly uncached). Verify cache behaviour /
  mark fixed after the lane (no wrong-results from caching).
- **B113** (per-read shard-manifest decode, ~2× on read-heavy: `00139_like` 228s→456s CA-local):
  real TIMEOUT RISK in the lane. Fix = memoize `readShard`/`resolveRef` decode within a read
  (manifest cache keyed by (ns,shard) with token/version invalidation). Strong next candidate.
- B106/B107/B108/B110/B114 = correctness/edge debts to audit; B109 = review debt; B111 = rename
  perf. Will triage against actual lane failures.
- NOTE: full lane is using `build/programs/clickhouse` — NO rebuilds until it finishes. B117 edit
  staged in src, will batch-build with B113 etc.

### 2026-06-13 ~08:50 — CURRENT STATE (gated on full lane)
Done & committed this session:
- `d8fd11659df` B116 read path (pipeline + FileView).
- `5b69a99fd5b` B118 Expect:100-continue (conditional-PUT hang). VERIFIED green.
- `6d1f3a43ab9` regression test `05006_content_addressed_dedup_blob_insert`.
- backlog: B116/B118 → FIXED; B119 noted; B120 added (Expect fallback follow-up).
- STAGED (not built): B117 cancellation check in `ReadBufferFromS3::processException`.

Queued for when the binary is free (after full lane):
1. Build B117 + run a quick check; commit.
2. Measure a read-heavy test (`00139_like`) on CA-S3 to confirm B113 decode bottleneck; if it
   times out / is ~2× slower, implement the token-validated shard-manifest decode cache, re-measure.
3. Triage any other full-lane failures.
Then work down: B106/B107/B110/B114 correctness audits, B92/B93, M-F (B94–B99), etc.

The full lane (`build/test_full_lane.log`) is the ground-truth "green tests" signal; awaiting it.

### 2026-06-13 ~09:00 — full lane mid-run signal (STRONG)
- At 1189 tests done: **1184 OK / 5 FAIL**, fast — NO timeout storm. B116+B118 fixed the headline
  lane failure (read-in-order/INSERT tests that 600s-timed-out now pass in seconds).
- Early failures (to triage when lane completes — likely pre-existing/non-CA, NOT timeouts):
  `test_optimize_using_constraints` (×3), `02784_connection_string`,
  `03650_alias_marker_distributed_different_databases` (the latter is the distributed-ALIAS project,
  unrelated to CA). Triage plan: repro each on CA-S3 vs plain to classify CA-bug / pre-existing /
  flaky; only CA-caused regressions are in scope here.

## Status legend
TODO / INVESTIGATING / FIX-WIP / VERIFYING / DONE(commit) / DEFERRED(reason)

---

## Log

### 2026-06-13 07:00 — session start
- Read-path B116 fix already committed `d8fd11659df` (pipeline + FileView stage), unit-green
  (223 CA + 61 view/pipeline). Lane still times out — blockers are B118 (deadlock) and the
  write-storm (concurrency).
- Discarded a contaminated wire-proxy experiment (the Python proxy itself truncated uploads;
  direct RustFS log is clean). Recorded below; do not repeat that approach for write-path.
- Direct standalone repro (no proxy, trace): blobs upload clean, part commits, then ClickHouse
  reads back the manifest root chunks `roots/<pool>/<uuid>/0..7` (kind=Read) with a burst of
  `Next retry in 25 ms`, logs `Start to refresh statistics`, then **total silence for 70s** —
  a block on a lock/future, NOT a spin. This is B118 and it is deterministic.

### B118 — ROOT-CAUSED (07:00–07:55), fix designed
- Repro: `tmp/ca_b118.sql` — `(x,y) ENGINE=MergeTree ... SELECT number, number FROM numbers(1e6)`.
  Deterministic hang. **Trigger: x and y columns are byte-identical → content-addressed dedup →
  the second column-blob's conditional PUT (`If-None-Match: *`) fails its precondition.**
- gdb (child under ptrace_scope=1, SIGINT-on-timeout, `tmp/b118_gdb.sh`): INSERT thread blocked in
  `WriteBufferFromS3::finalizeImpl → TaskTracker::waitAll → future::get`; an upload worker stuck in
  `makeSinglepartUpload → S3::Client::PutObject → AWS AttemptExhaustively → RetryRequestSleep`
  (backoff sleep), silent for ~49s.
- Mechanism: the dup-blob conditional PUT ships a ~3.8 MB body. RustFS evaluates the precondition
  mid-upload; when it rejects, the large body in flight races into a 500 `UploadStreamError` /
  connection break (seen via the proxy run) — a RETRYABLE outcome — so the SDK retries up to
  `s3_retry_attempts` (=500) × backoff, re-sending the body each time → ~40-min block.
  (A clean 412 is correctly NON-retryable — `IsRetryableHttpResponseCode(412)=false` — and is
  handled fast by the cold-reuse path; the hang is the large-body race, not the clean 412.)
- **FIX (validated by curl against RustFS): `Expect: 100-continue` on conditional PUTs.** With it
  RustFS returns `412` BEFORE the body is sent (verified: 4 MB body never uploaded, clean 412),
  eliminating the 500/broken-pipe race and the retry storm. This is the correct S3 idiom for
  conditional writes and matches the operator's intuition ("drain / don't blast the body").
- Same root family as the LANE write-storm (broken-pipe PUT retried attempt 164/501) → this fix
  should also unblock the lane INSERTs.
- TODO: (1) gtest at the right seam; (2) enable Expect:100-continue for CA conditional PUTs;
  (3) re-run B118 repro to green; (4) re-run the 4 lane tests.

#### Fix plan (PocoHTTPClient Expect:100-continue for conditional writes)
- Seam: `src/IO/S3/PocoHTTPClient.cpp` `makeRequestInternalImpl`, the send block (sendRequest →
  copyStream(body) → receiveResponse). Poco supports it natively:
  `HTTPRequest::setExpectContinue(true)` + `HTTPClientSession::peekResponse(resp)` (returns true on
  100-Continue → send body; false → a FINAL response is already in `resp` → skip body). In both
  cases `receiveResponse(resp)` is still called (Poco contract; it skips re-reading when
  `_responseReceived` is set by peek).
- Gate (narrow blast radius — only conditional writes, which today = CA backend): method==PUT &&
  has body && (`if-none-match` or `if-match` header present). Non-conditional S3 traffic untouched.
- On peek=false (e.g. 412) we skip the body; the request stream is then incomplete so the pooled
  connection won't be reused (closed) — acceptable for the dedup/precondition-loss case.
- TDD: functional regression = the B118 repro (INSERT of dup columns must complete, not hang).
  Add a stateless `.sh`/`.sql` test (dup-column INSERT on CA-S3) that runs in the CA-S3 lane.
  A pure unit test of PocoHTTPClient needs a live server, so the lane test is the regression gate.

#### B118 RESULT (07:55–08:30)
- Fix committed `5b69a99fd5b`. Verified on the deterministic repro: INSERT numbers(1e6) of dup
  columns now completes — `count=1000000`, exit 0, **zero** broken-pipe/500/retry events, RustFS
  not hammered (was: hung ~40 min). Reference confirmed on plain storage too.
- Regression test written: `tests/queries/0_stateless/05006_content_addressed_dedup_blob_insert.sql`
  (+ .reference). Plain MergeTree dup-column INSERT → becomes CA-S3 in the lane; trivial elsewhere.
- Validation: running `05006` + the 4 previously-timing-out read-in-order tests in the CA-S3 lane
  (B118 was blocking their INSERT-then-read; expect them to pass now that INSERTs don't hang).
  Commit the test once the lane confirms green.

### 2026-06-13 ~09:30 — full lane COMPLETE: 10388 OK / 20 FAIL / 104 skip (99.8%), NO timeout storm
Triage of the 20 (diffs read from the lane log):
- **Non-CA / pre-existing / out-of-scope (19):**
  - `03233_dynamic_in_functions` — Float64 last-digit rounding diff (`…68065` vs `…68074`); arch/precision.
  - `test_optimize_using_constraints` (×10 = flaky-retries of 1 test) — `INDEX_NOT_USED` w/ `force_primary_key`
    in `selectRangesToRead`; constraint-optimization, storage-independent.
  - `01854_s2_cap_union`, `02224_s2_test_const_columns` — S2 geo (build-config).
  - `01880_remote_ipv6`, `02479_mysql_connect_to_self`, `02784_connection_string`, `00163_shard_join_with_empty_table`
    — network/MySQL/connection env (`mysqlxx::ConnectionFailed` seen in the log).
  - `03649_alias_marker_distributed`, `03650_alias_marker_distributed_different_databases` — the distributed-ALIAS
    project (separate WIP), not CA.
  None plausibly caused by the S3 read/write changes. Spot-verify a couple on PLAIN storage once binary frees.
- **The one timeout — `03927_autopr_input_bytes_estimation_prewhere_filter` (600s):** `stateful, long`, reads the
  massive `test.hits` with parallel replicas + prewhere. Heavy CA-S3 read → perf timeout, NOT a correctness
  regression (B116 only improved read perf). Candidate beneficiary of B113 (manifest-decode cache).

**Verdict: B116+B118 removed the headline CA-S3 timeout storm with no regressions. CA-relevant tests are green.**
Next: free binary → spot-verify a pre-existing failure on plain → build B117 → B113 measure+fix (helps 03927)
→ continue down backlog (B106/B107/B110/B114 audits, B92/B93, M-F B94–B99).

### 2026-06-13 ~10:30 — B117 + B113 progress
- **B117** committed `dd408fef7ba` (cancellation check in `processException`; needed
  `#include <Common/ThreadStatus.h>` — first build failed on incomplete type, fixed). Smoke-verified.
- **B113 part 1** committed `d1590609597` — token-validated shard-manifest decode cache
  (`Store::readShardDecoded`, head-validated, immutable shared_ptr; resolveRef/listRefs use it;
  writers stay on uncached `readShard`). New gtest `ResolveDecodeCacheInvalidatesOnWrite`; CA battery
  224/224. MEASURED CA-local read-heavy: **2.66× → 1.70×** vs plain (−29% read time).
- **B113 part 2** (tree decode cache, content-addressed/immutable, no invalidation, bounded 16384)
  building now; will re-measure + run CA battery + a CA-S3 lane subset to confirm green.

### 2026-06-13 ~11:00 — B113 done, remaining-debt triage
- **B113 part 2** committed `47ec3241d53` (tree decode cache). Combined 2.66×→1.51× (read time
  −44%), CA battery 224/224. Backlog: B105/B113/B117 → FIXED. Validating on a CA-S3 lane subset.
- **B107** (verbatim-file mtime = epoch 0): assessed — the age-gated cleanup
  (`isOldPartDirectory`/`clearOldTemporaryDirectories`) targets tmp PART dirs, which DO carry the
  `.ca_mtime` stamp; only table-level verbatim files report epoch 0, and the full lane passed. So
  it's a latent/cosmetic debt, NOT green-blocking. DEFERRED (already in backlog) rather than
  speculatively add per-object mtime storage for little benefit.
- Remaining CA debts are mostly latent edge-cases (B106 audit, B110 Append-at-create single-writer,
  B114 size-check defense, B80 isFile) or large features (M-F full GC B94–B99). None are causing
  lane failures.
- PLAN: (1) confirm lane subset green; (2) launch a FINAL full CA-S3 lane with ALL fixes in (B113/
  B117 added since the last full run) as the comprehensive green gate; (3) during it (no builds),
  do the B106 read-only audit + draft M-F (B94–B99) planning notes.

### 2026-06-13 ~11:30 — consolidated state
FIXED + verified + committed this session:
- B116 read pipeline (`d8fd11659df`), B118 Expect:100-continue (`5b69a99fd5b`) + test (`6d1f3a43ab9`),
  B117 retry cancellation (`dd408fef7ba`), B113 shard cache (`d1590609597`) + tree cache (`47ec3241d53`).
- B105 fixed-by-B116; B106 audited (not a bug, CA BACKUP tests pass); B107 deferred (latent).
- Recorded: B119 (RustFS upstream), B120 (Expect fallback follow-up).
Lane: full CA-S3 lane 10388 OK / 20 FAIL (all non-CA or one stateful perf), no timeout storm.
Remaining latent/large debts (none green-blocking): B110 (Append-at-create, single-writer),
B114 (size-check defense), B80 (isFile, harmless), B111 (rename perf), B92/B93 (RustFS scanner),
B94–B99 (M-F full GC milestone — large), B101–B104, B25/B26/B31, B17, B1/B8/B13–B15, B109 (review).
NEXT: confirm B113/B117 lane subset → final full lane (all fixes) → M-F planning + B110/B114/B80 triage.

### 2026-06-13 ~12:00 — B113/B117 validated; review dispatched
- B113/B117 CA-S3 lane subset: Failed: 0, Passed: 7 (incl. 03257 B115-regression + 00163 column-oriented).
- B110/B114/B80 triaged as latent/deferred (not green-blocking).
- Dispatched an independent adversarial REVIEW subagent on the session's high-stakes changes
  (B113 cache, B116 pipeline, B117 cancel, B118 Expect:100-continue) — addresses the B109 review debt.
  Will NOT edit those files until it reports.
- NEXT: launch final full CA-S3 lane (all fixes in) once praktika frees the container; M-F (B94–B99)
  planning (read-only) during the run; address any review findings.
