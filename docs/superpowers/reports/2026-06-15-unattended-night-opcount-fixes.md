# Unattended night — CA op-count + soak fixes (2026-06-15 → )

**Mandate (verbatim intent):** finish spec+plan and **implement #1 (head-after-put → drop the follow-up HEAD) to completion** with tests+reviews. **#2 GC single-leader/retire-contention → backlog only.** **#3 RustFS overwrite-leak mitigation, #4 root_shards congestion (widen fanout), #5 fsck per-LIST timeout+progress → brainstorm/spec/plan/implement** likewise. Don't ask, don't stop; defer→backlog. Then a **12h soak with hourly healthchecks + reports**.

Branch: `cas-mergetree-poc`. Evidence base: `docs/superpowers/reports/2026-06-15-ca-soak-opcount-and-rustfs-findings.md`; backlog `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (B157/B158/B159).

## Status board
- [x] #1 head-after-put → PUT-response ETag — spec, impl, tests (WBS3 ETag), reviewed (clean), minor empty-ETag harden applied. Commits 2a13fe5cc0f + review-fix.
- [x] #5 fsck per-LIST progress + deadline — spec, impl, tests, reviewed (clean), minor walk-deadline harden applied. Commit d852ec53d34 + review-fix.
- [x] #4 root_shards widen — spec, impl, test, reviewed (clean). Commit d0194412d0b; soak conf = 64.
- [x] #3 RustFS overwrite-leak reaper — spec, script + sidecar, fs-test passed. Commit 6370753caa9. (Live validation in the soak.)
- [x] #2 GC single-leader / retire-contention → B160 (backlog only).
- [ ] 12h soak with hourly reports (NEXT)

### Progress 3 — soak launched (run #6), monitoring armed
All four implementations done, tested, reviewed, committed (2a13fe5cc0f #1, d852ec53d34 #5, d0194412d0b #4, 6370753caa9→busybox #3, 14cc9c90b9f review-fixes). #2 = B160. Soak bring-up hit TWO self-inflicted harness bugs (B161): a `#` comment inside the YAML `>`-folded createbucket `sh -c` (→ sh exit 2 → ch1 dependency fail) and a suspected named-volume destabilizer; both fixed (createbucket verify-loop, reaper reverted to a host-side busybox `docker exec` loop). **Soak run #6 LIVE** (seed 20260617, 12h, 6 workers, root_shards=64, reaper loop + poller + hourly Monitor `b9kwtfoae`).
**Hour-0 baseline (t=343s) — strongly positive:** ch1 250 parts/0.01s, ch2 210/0.01s; noref=0 broken=0 both; HEAD/PUT=1.20; **GC=0 failed/52 ok/0 retire-contention** (run#5: 300f/250rc — #4 fanout relieves B160); **err=0×412/0×503/0×broken-pipe** (run#5 had storms — #4 fixed the B158 64-permit congestion); replLag=0s; **du roots=7.4M** (run#5 hit 74G — reaper + spread), blobs=2.5G. #4 confirmed 64 shards; #1 in binary, RustFS returns matching PutObject/HEAD ETags (gate-safe), HEAD-reduction partial on this dedup-heavy workload (dedup `observeAndAdmit` HEADs remain — B161c follow-up).
Hourly summaries append to `logs/soak6_hourly.log`; the Monitor re-invokes me each hour to record + react.

### Hour-1 (t~66min): healthy + reaper cadence fix
ch1/ch2 responsive (0.01s), **0 no-ref / 0 broken**, replLag 0s. #4 holding: only **33×503, 0×412, 0×bp** (run#5 had storms); GC **20f/74ok/11rc** (~21% fail vs run#5 ~78% — fanout helps B160 but contention reappears as the pool grows). **roots/ grew to 34G** → diagnosed the reaper: it WORKS (34.5G→19.8G, ~15G reclaimed) but each pass is slow (5.5min over a large roots/) and the 5-min-sleep loop ran only ~every 14min, so the workload outran it. **Fix:** switched the reaper to a near-continuous loop (30s sleep) → roots/ back to 23G and bounded. Disk **316G free**; pool ~43G (roots 23G reaper-bounded + blobs 20G). Active *referenced* data only 1.73GiB/221 parts → blobs/ physical is unreferenced-blob churn awaiting CA GC (B160-contended); `MAX_POOL_GB=25` is referenced-bytes, no throttle yet. Disk-safety variables to watch: roots/ (reaper) + blobs/ physical (CA GC). Reaper-slow-pass + dedup-HEAD = B161 follow-ups.

### Reviews
Two parallel adversarial subagent reviews (#1; #5+#4) — NO blockers/majors. Two minors fixed: #1 empty-ETag → HEAD fallback (fail-safe, = old behavior); #5 deadline checked every ref (was every 64, skipped on pools <64 refs). 276 unit tests pass; only baseline `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` red (B140-deferred, pre-existing).

## Log
(appended chronologically below)

### Progress 2 (code complete for #1/#5/#4; #3 reaper done; #2 backlogged)
- **#1** code: `WriteBufferFromS3` captures final object ETag (singlepart PutObject + CompleteMultipartUpload) + `getResultObjectETag()`; base virtual; decorator forwards it; CA `nativeConditionalPut` + `NativeStreamingSink::finalize` record ETag-else-HEAD. Test `WBS3Test.ResultObjectETagIsCaptured` (mock S3 deterministic ETag, singlepart+multipart).
- **#5** code: `runFsck` gains `FsckProgress` + an overall `deadline` (checked between pages/refs → `TIMEOUT_EXCEEDED`); `CommandFsck` gains `--timeout` (default 600) + stderr progress (no threads — avoids std::async-join + interactive-detach hazards). Tests `CasFsck.ProgressCallbackFiresAndIsObservational`, `CasFsck.ExpiredDeadlineThrowsTimeout`. (Single-stuck-page bound = disk S3-retry config, documented.)
- **#4** code: `content_addressed_root_shards` setting → `MetadataStorageFactory` → CA ctor (`root_shards_=8` default) → `pool_config.root_shards`. Soak `storage_conf.xml` ca disk set to 64. Test `CaWiring.RootShardsConfigurable` (4 + default-8).
- **#3** reaper: `orphan_reaper.sh` + Debian sidecar on a shared `rustfs_data` volume; filesystem safety test passed; committed.
- **#2** GC livelock → B160 (committed).
- PROCESS NOTE: edited #5/#4 headers while the #1 build was mid-flight (risked an inconsistent binary). Recovered by killing it and starting ONE clean `ninja unit_tests_dbms clickhouse` (in progress) — ninja re-derives consistency from mtimes. Will validate #1+#5+#4 tests together, commit per-feature.

### Progress 1
- #1 spec written + refined (capability-based, not fail-loud — Native can be LocalObjectStorage which has no write-ETag). Code implemented: `WriteBufferFromS3` captures final object ETag (singlepart+multipart) + `getResultObjectETag()`; base virtual on `WriteBufferFromFileBase`; `WriteBufferFromFileDecorator` forwards it; CA backend `nativeConditionalPut` + `NativeStreamingSink::finalize` record the ETag (else HEAD). Unit test `WBS3Test.ResultObjectETagIsCaptured` added (mock S3 sets deterministic ETag). Build of `unit_tests_dbms`+`clickhouse` in progress.
- #2 GC livelock → **B160** backlog entry written (single-leader/lease-cadence/one-scheduler-per-pool fix directions; deferred per user).
- #5/#4/#3 specs written: `2026-06-15-ca-fsck-timeout-progress-design.md`, `…-ca-root-shards-widen-design.md`, `…-ca-rustfs-overwrite-leak-mitigation-design.md`. Plans/impl to follow after #1 validates.

### Start
Design for #1 approved by user (head-after-put → return the PUT/CompleteMultipartUpload object ETag as the WCreate token; drop the post-write HEAD; dedup-reuse `observeAndAdmit` HEAD untouched). Model-checked against `CaIncarnationCore.tla` (`WCreate` records `nextTok`, never a HEAD; `SabotageNoReobserve` proves the gate token is load-bearing → can't drop the token, only the HEAD). Beginning spec.
