# Unattended worklog — rev.7 disk lifecycle + writepath perf (2026-07-22)

Mode: subagent-driven-development, unattended, watchdog every ~20 min.
Plan: docs/superpowers/plans/2026-07-22-cas-disk-lifecycle-rev7.md (17 tasks), then the 11-point user program (see ledger).

## Timeline
- [start] Round opened at HEAD ed3dffb8d04. Ledger round appended. Dispatching Task 1 (05020 unique names, Phase A CI-unblock).
- [setup] Watchdog = cron e182893b (13,33,53 * * * * — every 20 min, session-only, 7-day auto-expire). The initial ScheduleWakeup was replaced by the cron and cancelled. impl-t1 (sonnet) dispatched on Task 1.
- [rotation] SDD ledger rotated: progress.md now holds ONLY the rev.7 round (15 lines); the RefTableState round + all pre-rev7 briefs/reports/review-packages moved to .superpowers/sdd/archive-pre-rev7/. Live files: progress.md, task-1-brief.md (impl-t1 in flight).
- [T1] impl DONE: 42ebf94810 (05020 unique names). Verification incl. same-server retry emulation + pre-fix repro (Code:668 Dormant). Reviewer dispatched. Ledger notes: praktika job name is "Stateless tests (arm_binary, parallel)" in this checkout; rotation raced the report file (restored).
- [T1] COMPLETE (42ebf94810; review clean w/ independent same-server repro). Dispatching T2 (04290/04295 unique names).
- [T2] impl DONE (6cb7a339c1b, 2/2 OK). Note: impl-t2 initially idled awaiting its own log-analyzer subagent; one nudge resumed it. Reviewer dispatched; build_asan warm-up started (symlink hazard ruled out: ci/tmp/clickhouse -> build/programs/clickhouse).
- [T2] COMPLETE (6cb7a339c1b, review clean). PHASE A DONE. impl-t3 (typed sentinel probe) in flight on warm build_asan; C++ tasks proceed sequentially (single ninja + SDD no-parallel-implementers rule).
- [T3] COMPLETE (probe da5c249b165 + fixes 5695fa7a363; 15/15 + 1143/1143). Opus review earned its keep: 3 Important (RESOURCE_NOT_FOUND dead branch on real S3; InstrumentedBackend not forwarding the new virtuals; present-path escape). Controller committed the fix after implementer bookkeeping stall.
- [T4] impl DONE (8b1df0cd914; fence 8/8, gate 1151/1151; funnel proof: all durable sites -> 3 raw call points; 1 conscious exclusion for reviewer adjudication). rev-t4 (opus) reviewing. T5 brief pre-extracted; T5 dispatch HELD until T4 review closes (same files CasPool/CasMountRuntime — fix-cycle overlap risk).
- [T4/4b] T4 code APPROVED (I1 -> 4b routed); 4b DONE (64135f52a60, 10/10 + 1153/1153). Delta re-review in flight with 2 sharp questions: Q1 per-attempt guard => counter zero-window between retries (controller-spotted); Q2 resurrectStaged/putOverwrite zero fence coupling (implementer-flagged live [C2] gap) -> 4c decision. T5 held.
- [T4 family] COMPLETE (3 commits; opus review caught plan under-scoping I1 -> 4b; controller caught per-attempt zero-window -> Q1 -> 4c hoist; Q2 -> backlog). Score: 4/17 tasks. T5 (lifecycle+identity gate, opus implementer) dispatched.
- [T5] impl DONE (33657ccebab; 5/5 + 1158/1158, happy-path byte-identical, 17 guards green). rev-t5 (opus) reviewing (~16 min in — normal). Carry-forwards for T6/T8 recorded in ledger.
- [T5] COMPLETE (33657ccebab + d00057186ab; review APPROVED; spec diagram note a1e5a28bd64). 5/17. Dispatching T6 (erasure proof, opus).
- [T6] COMPLETE (erasure proof; 8/8 + 1166/1166; review PASS/Approved 0 Crit/Imp; tripwire backlogged). 6/17. Dispatching T7 (bootstrap ordering).
- [T7] impl DONE (105f6f56740, 6/6+1172/1172) but review found I1 (observe/FSCK path mints _pool_meta over residual — real hole, implementer justification factually wrong) + I2 (cited artifacts stale and RED; reviewer re-verified truth). Fixture audit CLEAN, debris rule adjudicated safe. Fix cycle in flight (mint-permission gate + artifact refresh).
- [T7] COMPLETE (3-commit chain; review caught the observe/FSCK mint hole + stale-RED artifacts; polarity flipped to fail-closed). 7/17. Dispatching T8 (the central six-class gate — biggest task of the phase).
- [T8] impl DONE (d47ca818c3f; 9/9 + 1184/1184, zero existing-test edits). rev-t8 (opus) adjudicating deviation #3 (gc_quiescent_fn weakened to no-round-in-flight — controller doubts vs spec §2 "GC stopped" + T4's GC fence-gen exclusion) as the central question.
- [T8] COMPLETE (central gate; d47ca818c3f + 3f87abd81b8; review: wiring sound, rationale corrected to the real backstops; spec §2 fixed). 8/17. Dispatching T9 (empty-proof rule).
- [T9] COMPLETE (empty-proof rule; 6/6 + 1190/1190; review clean). 9/17. Dispatching T10 (FORGET verb — first verb-plumbing task; T10/T11 strictly sequential, shared plumbing files).
- [T10] impl DONE (503b7bbcdb8; FORGET protocol + found-and-fixed reclaim race; controller caught the under-testing Cas*:CA* gate citation, definitive re-run 1197/1197). Review APPROVED; regression-guard commit in flight (race test must go RED without trip#2).
- [T10] COMPLETE (FORGET; 2 commits; race guard red-demo genuine). 10/17. Dispatching T11 (GC STOP/START — same plumbing files, sequential).
- [interim] User-requested interim report delivered (10/17 closed, T11 in review; 5 real defects caught by the review layer; process rules stabilized). Continuing.
- [T11] COMPLETE (GC STOP/START; 2 commits; review clean, M1 refined by implementer correctly). 11/17. Dispatching T12 (introspection snapshot).
- [parallel] User ordered a whole-increment T1-11 review on Fable 5 xhigh — rev-increment running alongside impl-t12.
- [parallel] TLA+ track launched (tla-modeler): CaErasureProof + CaDiskLifecycle models, TLC-checked; T15 gated on the verdict. Three parallel tracks now: impl-t12 (SDD), rev-increment (T1-11 review), tla-modeler.
- [T12] impl DONE (380fa73669e; 6/6+1209/1209; [D5] one-helper consolidation). Increment review delivered: C1 CRITICAL (GC never exits on natural terminal — G2 zombie + foreign-lease-steal on Replaced) + I1/I3 doc fixes (controller, 6c4c05c9802) + I2/M1 -> impl-fix1 (parallel with rev-t12). Plan T14 block adapted to as-built schema (32a4993cbb3).
- [DECISION] User: FORGET-only v1 — excise the natural-erased proof stack (~-1500..2000 lines). Spec rev.8 + excision task before T13. TLA Model 1 dropped; Model 2 (lifecycle/FORGET) remains the T15 gate.
- [rev.8] SPEC COMMITTED (5f405d3f75c): FORGET-only — erasure asserted, never proven; 10-item excision list in §9. T12 fully closed (3 commits). Awaiting: fix1 (C1+M1+I2), TLA Model 2.
- [TLA] Model 2 GREEN (T15 gate OPEN; trip#2 machine-proven); Model 1 found real GC traces (F2/F3) + grace-load-bearing — final empirical validation of the FORGET-only excision. Models committed (b683013942d).
- [fix1] CLOSED (1fe585ea078, review APPROVED; TLA forms verified). Excision task in flight (spec rev.8 §9 + 3 adjudications). TLA track closed (2 commits; v1 model green = T15 gate evidence).
- [EXCISION] impl DONE (434f3214cec, -1095 net, gate 1200/1200 new baseline). rev-excise reviewing (over-removal hunt). After close: T13 resumes the main plan in the v1 (FORGET-only) configuration.
- [EXCISION] CLOSED (-1095 net; review clean). Tree = rev.8 FORGET-only v1, machine-checked lifecycle, baseline 1200. Resuming plan at T13.
- [T13] impl DONE (a4eca5f4c48; FSCK-on-running + manifest revalidation + advisory; 1203/1203). rev-t13 reviewing (error-during-revalidation adjudication is the sharp question).
- [T13] COMPLETE (FSCK hardening + T13b 05020 green-restore). 13/17. Dispatching T14 (teardown switch).
- [T14] COMPLETE (FORGET teardown; praktika x2 all-OK on rebuilt binary). 14/17. Dispatching T15 (the old-lifecycle rollback — TLA-gated, gate OPEN).
- [T15] in flight: STOP-rule caught a plan-map gap (05018/05019 verb tests) pre-edit; disposition issued (delete + trim/rename). C++ removal proceeding.
- [T15] main+disposition landed (1dc57a8d820 + f6a8950dee8; -363; gate 1200/1200; stateless 3/3+1). rev-t15's reachability trace PROVED the shutdown-window safety claim (startup all-or-nothing at registration; null-pool = teardown race only; running-server throw = designed non-Live lifecycle). Verdict composing.
- [T15] COMPLETE (rollback; both verdicts PASS). 15/17. Dispatching T16 (acceptance matrix — absorbs the adjudicated ledger).
- [T16] COMPLETE (acceptance closure; 1204/1204; 05022 verb-access landed). 16/17. Dispatching T17 (final gates + docs alignment) — the last plan task.

## 2026-07-23 12:40 — T17 implementer DONE, rev-t17 dispatched {#t17-impl-done}

`impl-t17` завершил финальную задачу плана: коммиты `87aeefac9bd` (comment-relics + 4 доп. stale-строки rev.7 в user-facing поверхностях — routed M8; + BACKLOG round-closure `{#disk-lifecycle-rev8-closure}`) и `f3c8528f64e` (spec rev.8 as-built alignment: `§3` GC self-exit predicate → трёхногий как в коде; `§5-7` planned→landed). Доказательства: gate `1204/1204`, stateless-семейство `6/6` дважды, всё на свежепересобранных бинарях (`build_asan` 12:28, `build/` 12:29). `§9` excision-список перепроверен посимвольно отдельным разведчиком — точен. Ревьюер `rev-t17` (sonnet) запущен.

## 2026-07-23 12:58 — PLAN COMPLETE 17/17; финальное ревью + пункт 2 запущены {#plan-complete-final-review}

T17 закрыт: fix-раунд `baf291f3192` (2 пропущенных строковых реликта — комментарии колонок `system.content_addressed_mounts` и enum-doc `IdentityLost`), ре-ревью `rev-t17` — оба вердикта Approved. **План rev.7/rev.8 выполнен целиком, 17/17, каждая задача с адверсариальным ревью.**

Запущены параллельно:
- ФИНАЛЬНОЕ whole-increment ревью (`fable`, пакет `ed3dffb8d04..baf291f3192`: 47 коммитов, 730 КБ) → отчёт `.superpowers/sdd/final-review-rev8-increment.md`; охота: композиция gate×fence×FORGET×GC×bootstrap, полнота/ущерб экзиции, правдивость сообщений, триаж Minor-долга, дисциплина upstream-поверхностей.
- Пункт 2 программы: ПОЛНЫЙ stateless, обычная конфигурация, asan-бинарь (симлинк `ci/tmp/clickhouse` → `build_asan/programs/clickhouse`, свежий 12:46) → лог `build_asan/test_full_stateless_normal_asan.log`. Правило на время прогона: НЕ пересобирать `build_asan`.

## 2026-07-23 13:20 — вердикт финального ревью: MERGEABLE AFTER FIXES {#final-review-verdict}

0 Critical / 2 Important / 3 Minor / 8 Notes. I-1: gate-then-lock TOCTOU в `runGarbageCollectionRoundNow`/`runOneGcRoundForTest` — ручной `GC RUN`, допущенный при `Live` и вставший за `gc_scheduler_mutex` FORGET-а, возобновляется на Vanished-пуле и гоняет полный GC-раунд по декомиссованному пулу. I-2: `runGcRebuildNow` вообще не берёт `gc_scheduler_mutex` — rebuild невидим для FORGET, durable-записи после «все потоки остановлены». Фикс обоих = lock-then-gate (паттерн `gcStart`) + регрессионные тесты с RED-демо. Minors: реликты «three Vanished values» (5 шт.), «erased»-комментарии, overclaim «temporarily unreachable» в `checkFenceOrThrow`. Остальные охоты (экзиция, правдивость, test-debt, upstream-дисциплина, коммит-сообщения) — чисто. Один fix-субагент (opus) запущен; ограничение — не трогать asan-бинарь `clickhouse` (его использует идущий stateless пункта 2), гейт только через `unit_tests_dbms`.

## 2026-07-23 14:05 — load-спайк RCA; фиксы финального ревью закоммичены; пункт 2 рестартован {#loadspike-fixes-restart}

13:20-13:33 — пик loadavg ~1043 (полный asan-stateless × clang-сборка фиксера): жертвы — `systemd-journald` (SIGABRT, авторестарт; это и был «зависший комп / core dump» c точки зрения пользователя) и сам stateless-раннер (таймауты тестов → «Break tests execution» → BrokenPipeError, клин). Прогон №1 признан средой-испорченным и убит (лог: `...ABORTED-loadspike.log`); НОВОЕ ПРАВИЛО: тяжёлые фазы только сериализованно. Фиксы финального ревью: `4baddb748cc` (I-1/I-2 lock-then-gate + 2 регрессионных теста, RED-демо с точными ассертами) + `0ff1cbfdb2c` (minors). Гейт `1206/1206`. Ре-ревью у финального ревьюера (read-only). Пункт 2, прогон №2 запущен 14:04 в одиночестве.

## 2026-07-23 14:10 — ФИНАЛЬНЫЙ ВЕРДИКТ: MERGEABLE AS-IS {#final-verdict-mergeable}

Ре-ревью финального ревьюера: оба Important-фикса подтверждены на финальном коде (I-1 — interleave-тест реально проходит через шов и настоящий `forgetDisk`; I-2 — независимая проверка `CasGc.cpp`: `rebuildBaseline` не имеет пути к `gc_scheduler_mutex`, дедлок исключён конструктивно), шов `setGcVerbAdmitWindowHookForTest` — ноль продакшн-вызовов, minors — полны, скоуп-крипа нет. **Инкремент rev.8 закрыт на `0ff1cbfdb2c`: 17/17 задач, whole-increment ревью, TLA-гейт, финальное ревью + фикс + ре-ревью.** Дальше: докатывается пункт 2 (прогон №2); после него — пересборка asan `clickhouse` (фиксы затрагивают CAS-пути, нужны CA-лейну) и пункт 3.

## 2026-07-23 14:47 — прогон №2 убит host-OOM; №3 запущен с --workers 8 {#run2-oom-run3-throttled}

№2 дошёл до ~4411/10990 (0 продуктовых красных) и был убит глобальным OOM хоста: asan-сервер вырос до ~38.8 ГБ RSS при полном параллелизме (nproc) + приаттаченный TPC-H, а на хосте живая сессия пользователя (Zoom). `oom_score_adj=1000` отработал как задумано — жертвой стал наш контейнер, не приложения пользователя. Прогон №3 — `--workers 8`: пиковая память ~4× ниже, свободные ядра пользователю, ETA ~2.5–3 ч.

## 2026-07-23 16:05 — №3 остановлен извне; по указанию пользователя №4 через nohup, 20 воркеров {#run4-nohup}

№3 убит внешним stop на `5410/10990` (не OOM). Пользователь указал: запускать через `nohup`, `--workers 20`. №4 запущен отвязанным от harness-задач (мониторинг — watchdog-cron; лог `..._run4.log`). Суммарно за №2+№3 тесты 1..~5400 прошли с единственным известным средовым красным (`02479_mysql_connect_to_self`, IPv6 в docker).
