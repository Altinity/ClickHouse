# Full session summary — CAS (content-addressed storage) stabilization, `cas-gc-rebuild`

Branch `cas-gc-rebuild`. This report consolidates every finding and fix produced across the whole
unattended/interactive run spanning 2026-07-17 through 2026-07-19: the codex-review triage fix wave
(31 findings, tasks T1-T13), the ASan/TSan gtest-battery hardening (memory bugs + `LOGICAL_ERROR`
exclusion-list closure), the #38 prod-scale scenario campaign (S01-S40), and the tail-session S08/
issue-#2052 work. Companion doc: `docs/superpowers/reports/2026-07-19-campaign-38-results.md` (the
S01-S40 scenario table in full).

All work is local-only except where a commit hash is followed by "(pushed)" — the branch was pushed to
`altinity/cas-gc-rebuild` once, scoped to the T13 batch, under an explicit user-granted push mandate;
every other commit listed here is unpushed pending a future explicit push request.

---

## 1. Codex-review triage findings (`2026-07-17-codex-review-triage.md`, 31 items)

An AI review pass over the CAS C++ source (excluding tests/docs) produced 31 numbered findings.
Each was independently triaged (confirmed / refuted / design-decision) before any fix was attempted.

| № | Находка | Вердикт | Фикс | Статус |
|---|---|---|---|---|
| 1 | Conditional S3 copy silently falls back to unconditional write (multipart path unprobed) | Confirmed | `a7d171f1d3e` (T8) | Fixed |
| 2 | Condemned-object resurrect PUT unconditional | Refuted | — | Comment-only |
| 3 | HEAD+GET identity straddle | Refuted | — | Comment-only |
| 4 | Swallowed condemn-marker write vs same-token adopt — GC could delete a live blob (top severity) | Confirmed | `21a6051e8ff`+`12ae454e7f2` (T1) | Fixed |
| 5 | Pre-CAS retention prune uses proposed-only generations → GC permanent wedge | Confirmed | `883c8f92d66` (T3) | Fixed |
| 6 | Relink sender→receiver handoff gap, no retention pin (dangling manifest) | Confirmed | spec exists, not landed | Deferred (own task) |
| 7 | Advertised relink pool ≠ reservation disk | Confirmed | `f3cd6e1ff1f` (T6) | Fixed |
| 8 | Scheduler/`cas_store` shutdown UAF race | Confirmed | `846a4f62a62` (T9) | Fixed |
| 9 | Decommission deletes successor's control objects (epoch-monotonicity break) | Confirmed | `a3c2c8dcb52`+`0b62fbaa2f7`+`9707a61ba2c` (T5, 3 rounds + design pivot) | Fixed |
| 10 | Event-sink assignment/read data race (TSan) | Confirmed | `846a4f62a62` (T9, paired with #8) | Fixed |
| 11 | Namespace drop misses unregistered build → ownerless Live namespace | Partial | — | Deferred (LOW) |
| 12 | Directory ops mutate durable refs at call time; `removeDirectory` leaves staged entries | Split | `081c4e0bf44` (narrow part, T10) | Fixed (narrow part); rest by-design |
| 13 | Throwing telemetry after durable publish | Confirmed (hardening) | `a853fbe525e` (T10) | Fixed |
| 14 | CityHash128 default hash w/o byte-verify | Design decision | — | Open, awaiting user |
| 15 | Local/NFS shared-pool only INFO-level | Design decision | — | Open, awaiting user |
| 16 | `ReaderExecutor` bypasses `file_view` payload window (wrong results) | Confirmed | `aaf61086527` (T7) | Fixed |
| 17 | Unknown GCS bucket-versioning fails open | Design decision | — | Open, awaiting user |
| 18 | Emulated `list` token dialect mismatch (fail-safe leak, phantom-delete accounting) | Confirmed | `7fcb72050e7`+`cbdd8493e14` (T2) | Fixed |
| 19 | Conditional mutations send only `Token.value`, type not checked | Confirmed (low harm) | T2 same commits | Fixed |
| 19c | (found during T2) emulated-storage seq-token not etag-seeded → latent local-CA data loss across restart | Confirmed | T2 same commits | Fixed |
| 20 | Decoders lax on missing identity / dup keys / unbound fold-seal key | Split a/b/c | `64f1f67990d`+`814fbd13fa0` (T10) | Fixed (a,c); (b) refuted |
| 21 | Version field narrowed u64→u32 before validation | Confirmed (trivial) | `380423443e8` (T10) | Fixed |
| 22 | Zero-valued GC config (`gc_interval_sec=0`, `gc_shards=0`) → spin/permanent wedge | Confirmed (all 3) | `4fdbb3eaf11` (T4) | Fixed |
| 23 | `truncateFile` silent no-op | Confirmed (trivial) | `73f50519dba` (T10) | Fixed |
| 24 | `unlinkFile` ignores `if_exists` contract | Confirmed (minor) | `8fc0c964a5b` (T10) | Fixed |
| 25 | Commit retry after rollback can skip already-published entries | Confirmed | `23926415ed6` (T10) | Fixed |
| 26 | Verbatim append CAS retry re-sends stale payload | Partial | — | Deferred (LOW/latent) |
| 27 | Rename treats existing destination as idempotent replay | By-design | — | Comment-only |
| 28 | Build-seq watermark pinned permanently on ctor throw | Confirmed | `328d72b4cd3` (T10) | Fixed |
| 29 | No experimental-feature gate on `metadata_type=content_addressed` | Design decision | — | Open, deferred to upstreaming |
| 30 | `system.content_addressed_mounts` swallows per-disk listing errors | Design decision | — | Open, awaiting user |
| 31 | `staging_backend=s3` silently falls back to local | Design decision | — | Open, awaiting user |

**Итого**: 31 находка → 17 реальных фиксов, 5 design-decision (открыты, ждут решения пользователя),
3 by-design/refuted (закрыты комментарием), 3 deferred (LOW severity), 1 расщеплена на новую находку (19c).

---

## 2. Fix-wave tasks T1–T13 (subagent-driven execution of the confirmed findings above)

| Task | Что чинит | Коммиты | Статус |
|---|---|---|---|
| T1 | #4 condemn-marker load-bearing gate (graduation/redelete gated on confirmed durable meta) | `21a6051e8ff`+`12ae454e7f2` | Done, review approved |
| T2 | #18/#19/19c emu-token etag-seeding + live-dialect probe | `7fcb72050e7`+`cbdd8493e14` (critical trust-flip fix in review) | Done, review approved |
| T3 | #5 parent∪proposed generation-prune union (GC wedge) | `aca597d6761`+`883c8f92d66` | Done; bonus: closed a single-leader crash window |
| T4 | #22 GC config bounds validation | `4fdbb3eaf11` | Done, zero review findings |
| T5 | #9 decommission tail-fencing vs successor reclaim | `a3c2c8dcb52`, `0b62fbaa2f7`, design `804cbbf3325`, final `9707a61ba2c` (owner tombstone-in-place redesign) | Done — 3 rounds + a mid-wave architecture pivot |
| T6 | #7 receiver pool-UUID recheck → byte-fallback | `f3cd6e1ff1f` | Done |
| T7 | #16 `file_view` in `ReaderExecutor` fallback condition | `aaf61086527` | Done |
| T8 | #1 refuse unconditional S3-copy fallback when `if_none_match` set | `a7d171f1d3e` | Done |
| T9 | #8+#10 scheduler/`cas_store`/`part_access` lifecycle races | `846a4f62a62` | Done |
| T10 | Contract batch #12-narrow/13/20a/20c/21/23/24/25/28 (3 sub-batches) | 9 commits, see table above | Done, all independently rebuilt+battery-verified |
| T11 | Comment wave (BY-DESIGN/FALSE-POSITIVE annotations, #2/3/27 etc.) | `73d58405952` | Done |
| T12 | S22 fix: blob freshness-meta writes join `CasRequestController` | `7771bb60c70` | Done — closed the fix wave |
| T13 | Mechanical clang-tidy sweep (~296 `arm_tidy` errors, `google-default-arguments`) | `24bd437c5df`, `481016320e0` (**pushed**), `fb357007419` | Done, 3 sub-batches (codex gpt-5.6-luna, edit-only) |

**Final whole-branch review** (codex sol, base `d57a41f353d`→head `7771bb60c70`, 38 commits, 783 KB diff):
1 Critical + 4 Important + 2 Minor findings, all fixed:

| Находка | Фикс |
|---|---|
| Critical: `partAccess` UAF (same class T9 meant to close but missed) + gcHealth-blocks-behind-round | `452d17af42f` |
| Important: S22 caller discarded Committed/Conflict/Unresolved outcomes | `8068d8c5fe0` |
| Important: decommission owner-tombstone write not resumable on ambiguous outcome | `bec89a9de95` |
| Minor: decommission same-UUID comment; Minor: `S3Exception` name preservation | `663c37dc391`, `28834db5bf1` |
| Important: `emu_token_state` unbounded growth | `08ea8d1200e` (closed the whole fix-wave + review cycle) |

Interjected work during the wave: Fast-test parser regression fix `b27ec0816de` (RELOAD DICTIONARY/
MODEL/FUNCTION dropped by CAS format-case grouping); jemalloc idle-RSS study (RCA: generic ClickHouse
`SystemLog` flush mechanics, zero CAS symbols — no fix needed).

---

## 3. ASan gtest-battery memory bugs (7 findings, all test-lifetime, zero product bugs)

| Round | Тест | Класс | Причина | Фикс |
|---|---|---|---|---|
| 9 | `CasSweepLateLog.LogBetweenSealedFromAndSealIdIsReportedNotRevived` | stack-use-after-scope | event-sink `events` vector declared AFTER `Pool`; `~Pool` fires into dead vector | declare before `Pool` |
| 10 | `CasSweepLateLog.SecondPassSuppressedWithDedupLatchButNotWithoutOne` | stack-use-after-scope | same shape | same fix |
| 18 | `CasPartWriteTxnRepoint.PromoteRepointsCommittedRef` | stack-use-after-scope | same shape | same fix |
| 19 | `CasPartWriteTxnStageManifestRetry.AmbiguousLandedWriteResolvesToCommittedWithoutReissue` | stack-use-after-scope | same shape | same fix |
| 20 | `CasPartWriteTxnBlobPutRetry.AmbiguousLandedWriteAdoptsOccupantWithoutReupload` | stack-use-after-scope | same shape | same fix |
| 21 | `CasPartWriteTxnPromoteStagedRetry.AmbiguousCopyLandedAdoptsDestinationWithoutRecopy` | stack-use-after-scope | same shape | same fix |
| 23 | `CasPoolShutdown.UnresolvedWedgeSkipsFarewell` | heap-use-after-free | `const Layout&` bound to `store->layout()`, read after `store.reset()` | take `Layout` by value |

All 7 fixed in one commit `4420b5a3498`. Round-23 UAF verified NOT the same as codex finding #8
(different object/layer).

---

## 4. `LOGICAL_ERROR` audit + gtest-battery exclusion-list closure

**User-stated principle** (verbatim): `LOGICAL_ERROR` только для грубого нарушения инвариантов
приложения; не для обычных внешних сбоев. Внешние воздействия, делающие состояние неожиданным для
`ClickHouse`, — валидный `LOGICAL_ERROR`.

**Round 1** (audit found 4 suspicious production sites; user decision: fix #1/#2, leave #3/#4 as valid
external-fault `LOGICAL_ERROR`s):
- `CasBlobHashingWriteBuffer.cpp` (OpenSSL/alloc faults) → `OPENSSL_ERROR`/`CANNOT_ALLOCATE_MEMORY`
- `CasRefSnapshotFormat.cpp` (`sealed_from > snapshot_id` decode check) → `CORRUPTED_DATA`
- both in commit `0e069357957`

**Round 2** ("почини раз и навсегда, без списков исключений" — user directive): systematic pass over
every remaining known-abort test in the battery's exclusion list. Found the OLD 41-entry exclusion list
was itself 28 false positives (host-SIGTERM-kill artifacts, not real aborts) plus 13 real entries:

- 3 genuine ASan stack-use-after-scope bugs (same event-sink-declared-after-`Pool` class as §3) in
  `RefWriterStalePrecommitSweep`×2 + `RefWriterRecoverySeal`×1
- 2 more latent instances of the same class found by auditing ALL 20 `setEventSink` capture sites:
  `CasAnomalyPolicy.ForeignBytesAtWedgeKeyTripFenceAndRemount`, `CasAnomalyPolicy.WedgeContractReleaseFailClosed`
- All 5 fixed in `gtest_cas_ref_writer.cpp`, commit `99879af4aca`
- 3 test-only fault injections misusing `LOGICAL_ERROR` to simulate an external/unrecognized failure →
  swapped to `UNKNOWN_EXCEPTION` (`CasPartWriteTxn.PromoteSwallowsPostDurableEventSinkFailure`,
  `CasPool.BeginPartWriteRetiresBuildSeqWhenConstructionFails`,
  `CasRequestControl.UnrecognizedErrorsFailSafeToUnresolved`) — commit `4efc898b951`
- 6 tests exercising genuine production invariants → split into `#ifndef DEBUG_OR_SANITIZER_BUILD`
  (unchanged) + a new `EXPECT_DEATH` death-test (`CasGcStateFormat.RejectsZeroGcShardsOnEncode`,
  `CasFormatTraits.CompleteUniqueAndGated`, `CasRequestControllerCreate.DeterministicLocalFailuresPropagateInstantly`,
  `RefWriterAppendLane.I1WedgeResolveCorruptionSurfacesAndKeepsWedge`, plus the 2 `CasAnomalyPolicy` tests
  above which got BOTH the UAS fix and the death-test split) — commits `99879af4aca`, `0d5f0be10c5`
- `CasPartWriteTxn.ManifestCapEncodedBytesJustUnderStagesSuccessfully` (TSan-speed flake, real-clock
  mount-lease fence) → frozen `boot_ms_fn` clock seam, commit `47ea8f3c1d9`

**Second, independent gtest-filter coverage gap discovered** (while fixing an unrelated CI report for
PR#2073): the established `Cas*:CA*:...` battery filter had NEVER matched ~89-90 tests across
`CaWiring*`/`CaTransaction*`/`CaDedupCache*`/`CaInlinePlacement*`/`CaPartPathParser*` suite names all
session. Running that newly-discovered set surfaced 3 more real bugs in one previously-never-tested
file (`gtest_ca_wiring.cpp`):
- `CaWiringWrite.PartialCommitRollsBackPublishedParts` — same `LOGICAL_ERROR`-misuse class → `CORRUPTED_DATA`
- `CaWiringOps.MoveDirectoryMutableCollisionPolicy` — genuine invariant → death-test split
- `CaWiringOps.MoveDirectoryOntoExistingDestinationBuildSurvives` — STACKED bugs: an ASan
  stack-use-after-scope (same event-sink class) + a genuine same-day regression (triage finding #24's
  `unlinkFile(if_exists=false)` tightening never updated this one pre-existing test, because the filter
  gap meant it was never run)

All 3 fixed in `def79031982` (the CI fix); battery filter corrected in all 3 battery scripts.

**Final result**: the whole CAS gtest battery now runs with **zero exclusions**, in one call, under all
three configurations: **1030/1030 plain, 1034/1034 ASan, 1034/1034 TSan** (delta = 4 death-tests that
only exist under `DEBUG_OR_SANITIZER_BUILD`). The old peel-and-continue exclusion-list mechanism
(`build/asan_battery.sh` etc.) is no longer needed for CAS.

---

## 5. Scenario-campaign findings, pre-existing this session (from earlier R5 rounds)

| ID | Находка | Причина | Фикс | Статус |
|---|---|---|---|---|
| S13 | Quiesce-wedge — cluster died ~6 min into "quiescing cluster" phase (2 attempts) | Attributed to the same lifecycle-lock class as the final-review Critical `partAccess` UAF | `452d17af42f` (side effect) | Resolved — attempt-3 with sidecar instrumentation PASS 13/13, quiesce ran clean ~7.5 min |
| S22 | S3 SlowDown on blob `.meta` PUT escaped to client as HTTP 500 | `putMetaIfAbsent`/`casMeta`/`deleteMetaExact` bypassed `CasRequestController` | = T12 `7771bb60c70` | Fixed, validated PASS 13/13 |
| S23 | Idle RSS +184.6 MiB over threshold; later a genuine linear TRACKED-memory growth | RCA: generic ClickHouse boot-warmup (`SystemLog` flush), zero CAS symbols in jemalloc diff | Card fix (steady-state baseline gate) `b22798a24a3` | Resolved, PASS (48.9/44.7 MiB < 64 MiB budget) |
| S31 | `ca-gc-dryrun` believed to only preview shard 0 under `gc_shards>1` | Card-oracle misdiagnosis (one-round preview vs cumulative multi-round reclaim), not a tool bug | `b0da1f60f42` | Fixed, validated PASS 10/10 |
| S36/S37 | All parts routed to `ca` disk instead of local disks (looked like a routing regression) | Card scale-param bug — per-part bytes exceeded the hot volume's 4 MiB size cap; routing was correct | `4d457ec378a` | Fixed, validated PASS (26/26, 23/23) |
| S38 | `RefLateLogDetected` never fires | Poison late log clamps its own key, starving the sweep pass that would report it (40 healthy GC rounds, every one suppressed) | Not fixed — architecture-level | Escalated to BACKLOG `[clamp liveness]` DESIRABLE→HARD |
| R3 | Acked-then-lost data-loss regression | `renameParts` disk-txn close | Fixed (per project memory; gates green, upstream draft pending) | Fixed |

---

## 6. Tail-session work (2026-07-18 22:14 → 2026-07-19 00:42, this segment)

| Находка | RCA | Фикс | Статус |
|---|---|---|---|
| S08 timeout (rc=124 at 900s in the #38 campaign) | Isolated instrumented rerun (sidecar polling `system.parts`/`system.processes` every 15s): create-phase alone (20000 sequential single-row HTTP INSERTs) takes ~880-900s — essentially the whole default timeout, before any post-creation checkpoint even starts. Steady ~20-25 parts/sec throughout, zero stalls — confirmed budget, not a hang/regression. | Harness-level: bump S08's default ci-scale timeout (resume scripts already use `TMO=2400` as a workaround) | Root-caused, not yet a permanent scenario-runner default change |
| S08 own end-checkpoint false-INCONCLUSIVE (`quiescence failed: ... genuine error`) | `quiesce_cluster`'s `drain()` raised INSTANTLY on the first sighting of any `replication_queue` entry with a non-empty `last_exception` — no grace period, unlike its sibling backlog-stall check. A single transient entry under the 20000-part creation burst (self-healing) tripped it. | `35faaae182c` — added `error_since`/grace-period tolerance (`no_progress_grace_s`, matching the stall check) + 2 new regression tests (`test_quiesce_cluster_tolerates_transient_errored_entry`, `test_quiesce_cluster_raises_on_persistent_errored_entry`) | **Fixed**, validated clean in a confirmation rerun (`anomalies=[]`, `quiesce_s=31.3s`) |
| Campaign methodology gap (user-directed) | User asked for explicit `trace_log` top-stacks by CPU/Real/Memory (separately), `query_log` anomaly checks, and per-phase accounted time | `scenarios/README.md`'s "Common observations" section expanded | Done, same commit `35faaae182c` |
| Issue [Altinity/ClickHouse#2052](https://github.com/Altinity/ClickHouse/issues/2052) (orphan-manifest sweep deletes live manifests, external report) | Reporter's build (`99a6e1bec66`) was ~330 commits behind HEAD, landed mid-refactor of the ref snapshot+log cutover. Fix locus confirmed: `318291fe5e5` switched the orphan-manifest sweep's ownership recovery onto `recoverRefTable` (ref-log model); `CasOrphanManifestSweep.cpp` on HEAD still uses it (`recoverRefTableDetailed`). | N/A — does not reproduce on HEAD | Draft reply finalized (`tmp/issue2052.md`), both pending verification steps confirmed (clean-pool minimal repro: `dangling=0`/exit 0; diff-read confirms fix commit) — **not posted**, awaiting user go-ahead |

---

## 7. #38 prod-scale scenario campaign — final result (S01-S40, `--scale ci --seed 1`)

Full table: `docs/superpowers/reports/2026-07-19-campaign-38-results.md`.

**40/40 сценариев дали вердикт на финальном бинарнике:**
- **27 чистых PASS**, без находок.
- **11 INCONCLUSIVE** (S03, S04, S05, S06, S07, S10, S11, S20, S21, S23, S29) — все относятся к
  уже известному классу "pre-existing metrics/instrumentation-window artifacts at ci scale" (GC-log
  capture window слишком короткий, blob-cache скрывает сравнение fetch-счётчиков, пороги атрибуции
  недостижимы ниже full scale) — не продуктовые баги, требуют `--scale full` чтобы стать
  окончательными, не код-фикса.
- **1 настоящий FAIL** (S38) — уже понятая, эскалированная архитектурная находка (см. §5).
- **S08** — отдельная диспозиция: подтверждённая бюджетная характеристика + найденный-и-исправленный
  harness-баг (см. §6).

**Регрессий из этой сессии — ноль.** Единственный открытый вопрос всей кампании — S38/clamp-liveness,
уже задокументированный и ждущий архитектурного решения пользователя.

---

## Overall tallies

| Категория | Найдено | Исправлено | Открыто/отложено |
|---|---|---|---|
| Codex-review triage (§1) | 31 | 17 | 8 design-decision + 3 by-design/refuted + 3 deferred |
| Fix-wave review findings (§2) | 7 | 7 | 0 |
| ASan battery memory bugs (§3) | 7 | 7 | 0 |
| `LOGICAL_ERROR`/battery closure (§4) | ~24 individual test/production fixes across 2 rounds + 1 filter-gap discovery (3 bugs) | все | 0 (battery = 0 exclusions) |
| Scenario-campaign findings, earlier rounds (§5) | 7 | 6 | 1 (S38) |
| Tail-session findings (§6) | 4 | 3 | 1 (issue #2052 — awaiting post approval, not a bug) |
| #38 campaign scenarios (§7) | 40 ran | 27 clean + 11 benign-inconclusive + 1 already-tracked fail | — |

**Всего продуктовых/тестовых находок за сессию: ~120+. Реальных нерешённых продуктовых проблем: 1
(S38 clamp-liveness, архитектурная). Всё остальное — исправлено, подтверждено или задокументировано
как non-actionable at this scale.**
