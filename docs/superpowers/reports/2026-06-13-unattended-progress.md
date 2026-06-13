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
