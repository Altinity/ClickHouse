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

Reviews: two parallel adversarial subagent reviews (#1; #5+#4) — NO blockers/majors. Two minors fixed: #1 empty-ETag → HEAD fallback (fail-safe, = old behavior); #5 deadline checked every ref (was every 64, skipped on pools <64 refs). 276 unit tests pass; only baseline `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` red (B140-deferred, pre-existing).

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
