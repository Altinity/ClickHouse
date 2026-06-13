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

### 2026-06-13 ~13:00 — independent review done + findings addressed
Dispatched an adversarial review subagent on B113/B116/B117/B118. 15 findings; 12 verified SAFE
(connection-reuse on 412, cache thread-safety/staleness self-correcting, tree-cache no-UAF,
cancellation no-data-loss, FileView bound translation + double-nesting). 3 actionable:
- **Finding 6 (memory leak)** FIXED `c1a26be818b`: shard_decode_cache now bounded (16384, clear on
  overflow) + evicted on dropNamespace. 224 gtests green.
- **Finding 5 (contract)** FIXED `c1a26be818b`: documented the same-token⟹same-bytes Backend
  precondition the decode cache relies on (holds for all backends in use). Probe-check noted.
- **Finding 1 (HIGH, Expect:100-continue fallback)** DEFERRED to B120: the clean poll-then-peek
  fallback is infeasible via the public Poco API (no socket() accessor; peekResponse closes on
  timeout); the proper fix is a cross-layer probe-gate / per-disk config flag — an attended change.
  Expect stays ON (required for B118; all CA stores in use honor it).
Verdict was FIX-BEFORE-SHIP on Findings 1+6; 6 fixed, 1 recorded with design for attended follow-up.

Caught + worked around a tooling bug: a Monitor whose command contained the literal "praktika run"
made `pgrep -f "praktika run"` match the monitor's own shell → false "still running" forever, so the
chained final-lane launch never fired. Stopped it (TaskStop) and launch the final lane with a
log-content monitor instead. LESSON: never pgrep a string that appears in the monitor's own command.

### Final full CA-S3 lane (all fixes incl. review) — RUNNING (`build/test_final_lane.log`).

### 2026-06-13 ~14:00 — FINAL full CA-S3 lane (all fixes incl. review): 10386 OK / 22 FAIL / 104 skip (99.79%)
Same result as the first full lane → review fixes (Finding 5/6) introduced NO regressions.
Failure triage (ZERO CA-correctness regressions):
- ~15 non-CA / pre-existing: `test_optimize_using_constraints` (×10, constraint-opt), `01854_s2`/`02224_s2`
  (geo), `02479_mysql`/`02784_connection_string` (env), `03233_dynamic` (float precision),
  `03649`/`03650_alias_marker` (distributed-ALIAS project), `01171_mv_..._isolation_long` (flaky/long).
- ~4 CA-S3 stateful heavy-read timeouts (B121): `00091_prewhere_two_conditions`, `03582_pr_read_in_order_hits`,
  `03634_autopr...`, `03800_autopr...` — all `stateful,long` reads of `test.hits`. The SET shuffles
  run-to-run (run 1 timed out `03927`, which PASSED here) → marginal/flaky near 600s, host-load-sensitive,
  NOT a regression. Recorded as B121 (perf: per-blob GET overhead on huge tables; fix = coalescing/packing).

## SESSION OUTCOME
Core mandate ACHIEVED: the CA-S3 lane went from a ~50-test timeout storm to 99.79% green with ZERO
CA-correctness regressions. The remaining failures are non-CA (env/geo/other-projects) or marginal
stateful-read perf timeouts (B121, a future read-coalescing/packing item).
Shipped + verified + reviewed: B116, B118 (+test), B117, B113 (×2), review Findings 5/6.
Backlog fully triaged: every open item is FIXED, AUDITED, DEFERRED-with-reason, or a recorded future
feature/milestone (M-F GC, B25 needs a decision, B17/B1/B26/B31 features). Persistent memory updated.
Did NOT rush the high-stakes GC delete path (M-C3/M-F) unattended — it needs the planned adversarial
review + fault-injection harness.

### 2026-06-13 ~14:30 — live validation of the GC delete path (M-C3) from the final lane
The final lane ran with GC enabled (5s interval). Server-log evidence over its full run:
- **~1132 `runRegularRound` rounds**, deleting TENS OF THOUSANDS of objects via exact-token deletes
  (e.g. round 52: candidates=28025 deleted=28023; round 51: 18303→18298, replaced=2;
  round 50: 26064 candidates, deleted=24692, **replaced=716**, spared>0).
- **ZERO GC-path safety alarms**: no LOGICAL_ERROR / FATAL / terminate / INV-* violation / TokenMismatch
  in any GC / incarnation / retire / fence / recheck / deleteExact context.
- The non-zero `spared`/`replaced` with `deleted < candidates` shows fence/recheck/retire correctly
  DECLINED to delete resurrected/in-flight objects (the model's "zombie delete after resurrect" /
  "spared-entry orphan" dangers) — and no error surfaced from those decisions.
This is strong real-world evidence the M-C3 GC delete path is solid under heavy DROP/mutation churn,
complementing the model-scenario gtest battery. (Does NOT cover M-F full-GC walk / debris / packs —
still unimplemented + unplanned.)

### Remaining work is now all LARGE/PLANNED or needs a decision — NOT safe to rush unattended:
- M-F (B94–B99 full GC walk, debris reclaim, **packs** — also B121's fix): unplanned milestone; needs
  design→plan→adversarial-review per the established M-C* discipline. Touches storage layout + GC.
- B25 (server-root ownership): needs an operator decision (shared-pool semantics) — recorded.
- B17 (encryption-at-rest), B1 (Replicated*MergeTree), B26/B31: feature milestones needing design.
Stopping point for SAFE unattended work reached: green-tests goal met, all well-scoped debts fixed/
triaged, high-stakes paths (read + GC delete) validated, changes independently reviewed.

### 2026-06-13 ~15:00 — second focused review dispatched (B109 write-path)
Found a blast-radius correction first: the Expect:100-continue gate (if-none-match/if-match) also
covers **Iceberg** conditional metadata writes (`DataLakes/Iceberg/Utils.cpp`), not just CA — safe
(same 412 outcome, beneficial on S3/MinIO), recorded in B120; corrects review Finding 3.
Dispatched a second adversarial review focused on the CA WRITE/COMMIT path
(`ContentAddressedTransaction`: commit atomicity, staging↔publish, autocommit finalize/cancel,
createHardLink carry-forward, move/rename, Append lost-update, error propagation) — the highest-stakes
un-reviewed code (data-writing correctness), addressing the rest of B109. Will fix any real findings.

### 2026-06-13 ~15:30 — write/commit-path review done (B109 advanced)
Second adversarial review (CA write/commit path). VALIDATED as solid: autocommit one-shots
(content blobs can't autocommit; finalize-once; cancel-never-publishes; truncated-blob guard),
createHardLink carry-forward (publish gate re-validates → no wrong-bytes / no re-upload),
same-name rewrite (clean replace), no-throw destructor. Findings recorded (NOT rushed — commit path
is high-stakes, fixes need attended design):
- **B122 (HIGH, latent):** no cross-ref commit atomicity — a `publish` throw mid multi-ref commit
  leaves earlier refs visible (RENAME TABLE same gap). Latent (publish doesn't throw in the lane;
  `parts` keyed by {ns,ref} so multi-ref IS reachable for partition ops). End-to-end harm depends on
  MergeTree recovery. Fix = stage-all-then-publish-with-compensation / recovery journal / one-ref-per-txn.
- **B123 (LOW/MED):** unguarded verbatim RMW (`moveFile` no CAS; `unlinkFile` fail-open no-op).
- **B124 (MED):** `moveDirectory` (dest-wins) vs `moveFile` (source-wins) opposite collision policies.
- B110 updated: append branch covers any non-part Append, not just mutation entries.
These are latent robustness gaps in the high-stakes commit path — recorded for ATTENDED design+fix,
consistent with the M-C* adversarial-review discipline; not rushed unattended.

### 2026-06-13 ~10:30 UTC — re-run all tests (+ integration), per request
- **Stateless CA-S3 lane (3rd full run)**: Failed: 21, Passed: 10387, Skipped: 104 (99.8%) — stable,
  same categories (test_optimize_using_constraints ×10, s2 geo, mysql/connection, alias project,
  03233 float, + B121 stateful-read flaky timeouts 00091/03800/03927). Zero CA-correctness regressions
  across THREE full runs.
- **Integration**: launched `test_content_addressed_s3` + `test_content_addressed_gc_s3` (the CA
  integration tests). CAVEAT: these use minio, which CA's probe rejects (the B93 reason RustFS replaced
  it in the stateless lane); they predate the probe enforcement, so a probe-fail at startup would be a
  PRE-EXISTING integration-needs-RustFS gap (parallel to B93), not a regression. Triaging on result.

### 2026-06-13 ~11:05 UTC — integration result
CA integration tests (`test_content_addressed_s3`, `test_content_addressed_gc_s3`): 3 ERRORS, ALL at
the SHARED minio bring-up fixture (`cluster.py` 'Trying to connect to Minio' → Connection refused at
the container :9001) — BEFORE the clickhouse node / CA code ran. Generic local integration-env issue
(minio unreachable in the local harness — TLS/cert/networking), affects all minio integration tests,
NOT CA-specific, NOT a regression. Plus: these tests use minio, which CA's probe rejects → stale,
need RustFS (recorded B125, parallel to B93). CA-over-S3 remains validated by the stateless lane
(RustFS, 3× green) + 224 gtests + live GC-delete validation + 2 reviews.

### 2026-06-13 ~11:20 UTC — non-CA regression checks (s3-minio) + arm/amd clarification
- CLARIFICATION (raised by operator): host is **x86_64 (AMD)**, binary is **x86-64**. All test runs
  used OUR x86 binary — the lane symlinks `ci/tmp/clickhouse -> build/programs/clickhouse`. The
  praktika label `arm_binary` is just the **plain-release-binary test profile** (the only non-sanitizer
  stateless profile in ci/defs; all `amd_*` stateless variants are ASan/TSan/MSan/debug/coverage). It
  selects the test config + the CI artifact to require (ignored locally); it does NOT run an ARM binary.
  So prior green runs are valid on our AMD binary.
- To run the NORMAL (non-CA) s3-minio stateless lane on our plain x86 binary, added a parametrization
  `arm_binary, s3 storage, parallel` in ci/defs/job_configs.py (plain-binary profile + normal-s3-minio;
  minio started locally by the lane's setup_minio.sh — NOT the integration docker-compose minio that
  failed). Smoke (4 tests) green. Full lane RUNNING (`build/test_s3minio_full.log`) — confirms the
  shared S3-client changes (Expect:100-continue, ReadBufferFromS3 cancellation) don't regress non-CA.
  (NB: normal MergeTree-on-s3 doesn't set conditional headers, so Expect doesn't fire there; the
  non-CA Expect surface is Iceberg — to be covered via integration.)

### 2026-06-13 ~11:40 UTC — full s3-minio (non-CA) lane: NO REGRESSIONS
Result: Failed: 19, Passed: 10269, Skipped: 224. The 19 failures are the EXACT SAME non-CA categories
as the CA-S3 lane — test_optimize_using_constraints ×10 (constraint-opt), S2 geo (01854/02224),
env/network (00163/01880/02479/02784), float precision (03233), distributed-ALIAS project (03649/03650).
NO new failures, NO timeouts, NO s3-specific breakage. Since they fail IDENTICALLY on the normal s3
path (which the CA-only changes don't touch), this is DEFINITIVE: the shared S3-client changes
(Expect:100-continue, ReadBufferFromS3 cancellation) do NOT regress the non-CA / s3-minio path.
Integration tests (2 CA + test_merge_tree_s3 non-CA) re-run with minio — pending (also reveals whether
this minio version enforces conditional ops, i.e. whether CA works on it at all).

### 2026-06-13 ~12:00 UTC — integration with minio: NO non-CA regression; CA fail-closes on minio (by design)
2nd integration run (minio connectivity worked this time — earlier failure was transient): **22 passed,
3 skipped, 3 errors**. The 3 errors are ALL the CA tests, erroring at CA-disk load with
`CasProbe: deleteExact with a wrong token was not TokenMismatch — backend does not enforce conditional
deletes (NOT_IMPLEMENTED)` (CasProbe.cpp:146 → Store::open). I.e. CA's safety probe CORRECTLY rejects
minio (which honors a mismatched-token DELETE — unsafe for the incarnation protocol). Fail-closed,
by design, pre-existing (B125 — CA integration tests need RustFS), NOT a regression.
The non-CA `test_merge_tree_s3` (+ 21 others) PASSED → no regression on non-CA s3 integration.

## OVERALL no-regression verdict (per operator request)
- non-CA s3-minio STATELESS: 10269 passed, only the same pre-existing non-CA failures as every lane.
- non-CA s3 INTEGRATION (test_merge_tree_s3 + 21): passed.
- CA-S3 STATELESS (RustFS): 99.8%, 3× stable, zero CA-correctness regressions.
=> The shared S3-client changes do NOT regress the non-CA / minio paths. The only CA "failures" are
the CA integration tests fail-closing on minio (correct safety behavior; they need RustFS = B125).

## 2026-06-13 ~13:35 UTC — acting on umbrella-review findings (unattended; skipping #2 docs-cleanup)
Plan (verified batches): (A) observeAndAdmit underflow guard + ReadBufferFromFileView exception-safety
+ trivial nits; (B) narrow Expect:100-continue to a CA-only signal; (C) operability (scratch-path
resolve, emulated-mode shared-FS warning, getLastModified epoch-0 fallback) + small hardening; (D) B122
commit atomicity (compensating rollback) + gtest; (E) assess B125 RustFS multi-node integration.

## 2026-06-13 ~13:50 UTC — batches A–D landed
- **Batch A** (`c9eae8fc7d1`): `observeAndAdmit` blob-underflow guard (CORRUPTED_DATA if object < header_len);
  `ReadBufferFromFileView::executeWithOriginalBuffer` exception-safety (restore swap on throw);
  removed duplicate `MOVE_PARTITION` from the PlainRewritable supported-commands list.
- **Batch B** (`55b4b3e580f`): narrowed `Expect:100-continue` to LARGE conditional writes (≥1 MiB body
  AND `if-none-match`/`if-match`) — excludes Iceberg metadata + small CA manifest writes. (B120 follow-up
  still open: graceful fallback for a store that ignores Expect.)
- **Batch C** (`ac0d5453210`): anchor a RELATIVE `cas_scratch_path` to the server data path (not CWD);
  `LOG_WARNING` in startup() when the backend runs EmulatedSingleProcess (local pool) — a shared/NFS
  pool between servers would break CAS invariants silently and the probe can't detect it. (review #1/B25)
- **Batch D** (`08add5c0d4f`): **B122 commit atomicity FIXED.** `commit()` now tracks the refs it creates
  and best-effort `dropRef`s them on any mid-loop exception (all-or-nothing); fail-closed (never drops a
  pre-existing ref; updateRefPayload one-shots not rolled back). TDD via `FaultyLocalObjectStorage` that
  fails the 2nd shard-manifest publish — confirmed failing pre-fix, passing post-fix. Full Cas*/Ca*
  battery green (225). **Operator Q (TLA+):** answered — partial commit is NOT a protocol-invariant
  violation (publish/dropRef each gate-checked + journalled; debris GC-reclaimable; §9 invariants hold),
  it's a wiring-layer atomicity gap ABOVE the model's abstraction line, so the fix needs a wiring-layer
  fault-injection gtest, NOT a TLA+ extension. RENAME TABLE's multi-namespace move has the same shape but
  a different (longer) fix → split out as **B126**.

Remaining review action items to work next (unattended): TOCTOU resolveRef→dropRef catch
FILE_DOESNT_EXIST; resurrect recursion bound; `.ca_mtime` std::stoull wrap guard; TLV/MAX_HEADER_LEN
guards; NativeStreamingSink empty-token assert; `tryGetInFlightStorageObjects` physicalKey; unbounded
registry guard (#4). Larger/deferred: #2 dynamic_cast-to-concrete→virtual dispatch, docs cleanup.
