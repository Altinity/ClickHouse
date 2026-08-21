# Триаж umbrella review от 2026-08-05 {#fable-review-triage}

Источник: `docs/superpowers/cas/random/fable-revew-20250805.md` — обзор диапазона
`altinity/antalya-26.6` (`5e8eaeb4d7d`) → `feature/antalya-26.6/CAS` (`056488b47a0`), 14 параллельных
ревьюеров, вердикт «request changes» с 4 блокерами и 9 major-пунктами.

Здесь каждый вывод перепроверен против текущего HEAD ветки `cas-gc-rebuild` (2026-08-21+): за
прошедшие с обзора недели уехали rev.8 disk-lifecycle (17/17), writepath stage-1 (14/14),
TXN-ONE-PIPELINE, ack-floor GC и прочее, поэтому часть выводов устарела.

Статусы: **исправлено** (было реально, закрыто — с коммитом) · **подтверждено** (всё ещё на HEAD) ·
**частично** (закрыта часть) · **не подтвердилось** (при перепроверке оказалось неверным) ·
**by-design** (осознанная позиция). Приоритет: P1 (до релиза) · P2 · P3 · — .

## Блокеры и major {#blockers-major}

| ID | Тема | Статус на HEAD | Приоритет | До релиза? | Где отслеживается |
|----|------|----------------|-----------|------------|-------------------|
| B1 | `~Gc` рвёт condemn-мьютекс до дренажа `meta_pool` (UAF-класс) | подтверждено | P1 | да | — (нигде не отслеживается: нет ни в `BACKLOG.md`, ни в `BACKLOG/gc.md`, ни в `final-checks-todo.md`, ни в `2031-triage.md`) |
| B2 | Два `LOGICAL_ERROR`-abort, достижимые сменой окружения (`claim()`, delete-marker) | подтверждено | P1 | да | 2b — `docs/superpowers/cas/BACKLOG/formats-and-storage.md:139` {#versioning-enabled-after-mount} (из 2031-triage CAS-029, там P3); 2a — не отслеживается нигде |
| B3 | Ad hoc `disk(metadata_type=cas)` обходит привилегии `SYSTEM CAS` | подтверждено | P1 | да | — (по существу не отслеживается; ближайшее смежное — `docs/superpowers/cas/BACKLOG/docs-and-cleanup.md:99` {#pool-trust-boundary-undocumented}, но это ДОК-пункт про границу доверия пула, а не про gate на `custom_disk`. 2031-triage CAS-132 (`2031-triage.md:150`, `:5729`) касается темы вскользь и классифицирован not-a-bug) |
| B4 | Гонка concurrent `FETCH PARTITION`: тест выключен, трекера нет (B66a) | частично | P3 | нет | `docs/superpowers/cas/BACKLOG/formats-and-storage.md:55` — **[B66a]** внутри раздела {#local-backend} (`:33-64`); связанные пункты там же — `[disk-error-audit]` (`:36-51`, чей фикс закрывает механизм B66a) и `[B26 / B135]` (`:52-54`) |
| M5 | `ContentAddressedTransaction` стейджит записи манифеста за O(F²) | подтверждено | P3 | нет | `docs/superpowers/cas/BACKLOG/performance.md:436` {#staging-vector-quadratic-path-scans} (заведён триажем 2031, CAS-1… |
| M6 | Blob-upload pool: сырая ссылка убегает из-под лока; `clickhouse-local` рушит порядок teardown | подтверждено | P2 | нет | — (нигде: в `BACKLOG.md`, `BACKLOG/*.md`, `final-checks-todo.md` про teardown blob-пула нет ничего; ближайшее в 2031-… |
| M7 | Инлайновые CAS-диски не сносятся на `DROP TABLE` (утечка lease + потоков) | частично | P2 | нет | `docs/superpowers/cas/BACKLOG/mounts-and-lifecycle.md:25` {#disk-lifecycle-rev8-closure} — раздел «CAS disk lifecycle… |
| M8 | Сетевой деструктор `Pool` может выполняться под `pointer_mutex` | подтверждено | P3 | нет | — (нигде: `grep -ni pointer_mutex` по `BACKLOG.md`, `BACKLOG/*.md`, `final-checks-todo.md`, `2031-triage.md` даёт тол… |
| M9 | GCS `gcp_oauth` переписывает ВСЕ запросы/ответы, не только условные записи | исправлено | P3 | нет | остаток (живая валидация) — `docs/superpowers/cas/BACKLOG/formats-and-storage.md:22-28` {#backends}: «GATE #1: Azure … |
| M10 | Disk-transaction contract трогает 16 общих файлов MergeTree — нужен динамический non-CAS прогон | частично | P2 | нет | нигде — по существу не отслеживается. Греп по `docs/superpowers/cas/BACKLOG.md`, `BACKLOG/*.md`, `final-checks-todo.m… |
| M11 | Кросс-дисковый `MOVE PARTITION` (CAS ↔ non-CAS) достижим, но помечен непроверенным | частично | P2 | нет | направление В CA — `docs/superpowers/cas/BACKLOG/replication.md:15` **[move-part-to-ca-architecturally-unimplemented]… |
| M12 | Публичные доки ссылаются на несуществующие артефакты и недоописывают настройки | подтверждено | P2 | нет | нигде. Ни в `docs/superpowers/cas/BACKLOG.md` (включая `## Inbox`), ни в `BACKLOG/docs-and-cleanup.md`, ни в `final-c… |
| M13 | Пробелы наблюдаемости и покрытия под собственные классы риска | подтверждено | P2 | нет | отслеживается ЧАСТИЧНО и по кускам. `gc_scheduler_running` — да: `docs/superpowers/cas/BACKLOG/operability-and-intros… |

## Minor issues {#minor}

| ID | Статус | Приоритет | До релиза? | Где отслеживается | Суть |
|----|--------|-----------|------------|-------------------|------|
| m1 | подтверждено | P3 | нет | — (смежное: `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:114` {#fsck-… | `clickhouse-disks cas-fsck` по-прежнему бросает `BAD_ARGUMENTS` на четырёх находках целостности — тот же код, что и на кривом флаге. |
| m2 | подтверждено (смежно с дубликат CAS-096) | P3 | нет | частично — `docs/superpowers/cas/2031-triage.md` CAS-096 {#refplan-dead-drop-counters},… | офлайновый `cas-gc-rebuild` печатает 9 полей отчёта, но не два «дисастер-специфичных» — `virgin_by_enumeration` и `adopted_seal_generation`. |
| m3 | подтверждено | P3 | нет | — (нигде: в `BACKLOG.md`, `BACKLOG/*.md`, `final-checks-todo.md`, `2031-triage.md` про … | `SYSTEM CAS DROP POOL MEMBER` по-прежнему требует строковый литерал для диска, тогда как шесть соседних глаголов принимают bare identifier. |
| m4 | подтверждено | P3 | нет | — (не отслеживается; `BACKLOG.md:723` {#issue-2211-gc-run-follower-noop} и `final-check… | `SYSTEM CAS GC RUN` без диска по-прежнему итерирует имена дисков без дедупа по нижележащему CA-указателю → двойной раунд на cache-over-CAS. |
| m5 | дубликат CAS-033 | P2 | нет (пред-релизная половина висит на CAS-040) | `docs/superpowers/cas/2031-triage.md` CAS-033 {#ckpt-damage-no-repair-path} (by-design,… | порча тела манифеста по-прежнему валит весь раунд GC (нет try/catch), в отличие от per-namespace `HOLD` для ref-логов — но это уже покрыто CAS-033/CAS-040. |
| m6 | подтверждено | P3 | нет | — (грепы по `BACKLOG.md`, `BACKLOG/*.md`, `final-checks-todo.md`, `2031-triage.md` на `… | `system.cas_gc_log` по-прежнему на руками пронумерованных `Enum8`, тогда как соседний `cas_log` использует `LowCardinality(String)`. |
| m7 | подтверждено | P3 | нет | — (не отслеживается) | `Primitives/CasTypes.h` по-прежнему включает `Formats/CasFormat.h` и зовёт `storedSuffix`, нарушая собственное правило «Primitives — zero outward dependencies». |
| m8 | подтверждено | P3 | нет | — (смежно `2031-triage.md` CAS-037 {#numeric-parse-and-window-wrap}, но там про заворот… | `ReadBufferFromFileView::resizeWorkingBuffer` по-прежнему усекает буфер, не трогая `pos`, — латентный выход за границы, недостижимый текущими вызывающими. |
| m9 | дубликат CAS-038 | P3 | нет | `docs/superpowers/cas/2031-triage.md` CAS-038 {#decoder-optional-field-residuals} (внут… | асимметрия обязательных полей в `Formats/` жива в конкретном виде (`tv` не требуется в outcome-логе), но класс уже разобран и затрекан триажем CAS-038. |
| m10 | подтверждено | P3 | нет | `docs/superpowers/cas/BACKLOG.md:190` {#disks-exit-code-upstream} | `clickhouse-disks --query` действительно теперь возвращает ненулевой код при ошибке команды — breaking change для `set -e`-скриптов, релиз-нота всё ещё не написана. |
| m11 | подтверждено | P3 | нет | `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:67` {#lazy-load-tables-d… | одно-табличные `SYSTEM`-глаголы теперь разворачивают `StorageTableProxy` и материализуют ленивую таблицу — юзер-видимое изменение поведения, changelog-строки нет. |

## Nits {#nits}

| ID | Статус | Приоритет | До релиза? | Где отслеживается | Суть |
|----|--------|-----------|------------|-------------------|------|
| n1 | ⏳ | — | — | — | — |
| n2 | ⏳ | — | — | — | — |
| n3 | ⏳ | — | — | — | — |
| n4 | ⏳ | — | — | — | — |
| n5 | ⏳ | — | — | — | — |
| n6 | ⏳ | — | — | — | — |
| n7 | ⏳ | — | — | — | — |
| n8 | ⏳ | — | — | — | — |
| n9 | ⏳ | — | — | — | — |
| n10 | ⏳ | — | — | — | — |
| n11 | ⏳ | — | — | — | — |
| n12 | ⏳ | — | — | — | — |
| n13 | ⏳ | — | — | — | — |
| n14 | ⏳ | — | — | — | — |
| n15 | ⏳ | — | — | — | — |

## Needs verification — что обзор оставил открытым {#needs-verification}

| ID | Статус | Приоритет | До релиза? | Где отслеживается | Суть |
|----|--------|-----------|------------|-------------------|------|
| V1 | ⏳ | — | — | — | — |
| V2 | ⏳ | — | — | — | — |
| V3 | ⏳ | — | — | — | — |
| V4 | ⏳ | — | — | — | — |
| V5 | ⏳ | — | — | — | — |
| V6 | ⏳ | — | — | — | — |
| V7 | ⏳ | — | — | — | — |

---

# Детали {#details}

## B1 — `~Gc` рвёт condemn-мьютекс до дренажа `meta_pool` (UAF-класс) (подтверждено, P1) {#b1}

**`~Gc` по-прежнему отсутствует, `meta_pool` объявлен ДО `condemn_marker_mutex`/`condemn_markers_confirmed` — на исключении в раунде живой воркер пула лочит уже разрушенный мьютекс (UB класса heap corruption).**

Что было заявлено (ревью 2026-08-05, blocker 1): у `Gc` нет собственного деструктора; `meta_pool` объявлен раньше `condemn_marker_mutex`/`condemn_markers_confirmed`, поэтому мьютекс и множество разрушаются, пока пул ещё жив; если `fold()` бросает после хотя бы одного `scheduleCondemnMarkerWrite`, но до `meta_pool->wait()`, catch-all планировщика глотает исключение, `stop()` джойнит только `thread`/`hb_thread`, и разрушение `Gc` происходит при живом воркере.

Что на HEAD (`d26abf94dfc`, ветка `cas-gc-rebuild`), после переезда файлов в `Gc/`:

1. Деструктора нет. `grep -rn '~Gc\b' src/.../ContentAddressed/Gc/` даёт ровно одно попадание — и это КОММЕНТАРИЙ, а не код:
   `Gc/CasGc.cpp:391`: `/// property -- the pool is a member of the same `Gc` and is joined by `~Gc` before the atomic dies.`
   То есть код по-прежнему сам себе противоречит: комментарий ссылается на несуществующий `~Gc`.

2. Порядок объявления членов не изменился (`Gc/CasGc.h`):
   - `:950` `std::unique_ptr<ThreadPool> meta_pool;`
   - `:957-958` `std::atomic<uint64_t> meta_jobs_scheduled_{0}; std::atomic<uint64_t> meta_jobs_completed_{0};`
   - `:967` `std::mutex condemn_marker_mutex;`
   - `:968` `std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;`
   Разрушение идёт в обратном порядке: сначала `condemn_markers_confirmed`, затем `condemn_marker_mutex`, затем атомики, и только в конце `meta_pool` (чей `~ThreadPool` и джойнит воркеров). Воркер в этот момент выполняет `noteCondemnMarkerDurable` (`Gc/CasGc.cpp:442-446`): `std::lock_guard lock(condemn_marker_mutex); condemn_markers_confirmed.emplace(ref, token.value);` — по уже разрушенным объектам.

3. Единственный дренаж — по-прежнему только успешный путь: `Gc/CasGc.cpp:1023-1028`, фаза `meta_pool_wait`, `meta_pool->wait();`. Ни одного `SCOPE_EXIT` с дренажом в `CasGc.cpp` нет (`grep -n SCOPE_EXIT` по файлу — 0 попаданий).

4. Точки планирования сохранились: `Gc/CasGc.cpp:962`, `:1856`, `:1911` вызывают `scheduleCondemnMarkerWrite`, который на `:417` делает `meta_pool->scheduleOrThrowOnError(run)`.

5. Планировщик не изменил поведения: `Gc/CasGcScheduler.cpp:342-348` — catch-all в цикле раундов («Idempotent round - the next tick retries; failures must never kill the pacing thread»), без дренажа пула; `CasGcScheduler::stop()` (`:75-93`) джойнит только `thread` и `hb_thread`; `~CasGcScheduler` (`:60-63`) вызывает `stop()` и затем разрушает member-by-value `Cas::Gc gc;` (`CasGcScheduler.h:190`). `round_in_flight` сбрасывается через `SCOPE_EXIT` (`:129-130`) и на пути исключения тоже, так что `isQuiescent()` рапортует «тихо» при живом воркере — ровно как в ревью.

Фиксящего коммита нет: `git log -S "condemn_marker_mutex" -- Gc/CasGc.h` даёт единственный коммит `21a6051e8ff` («cas: gc — condemn marker is load-bearing: graduation gated on confirmed durable meta»), то есть коммит ВВЕДЕНИЯ, не исправления.

Что осталось: весь дефект целиком. Работы rev.8 (UNMOUNT stops-all-background, GC stop/start) сделали разрушение `Gc` штатным и более частым событием (не только при завершении сервера), что скорее повышает достижимость, чем понижает. Исправление то же, что предложено в ревью: явный `~Gc`, который первым делает `meta_pool->wait()`/сбрасывает `meta_pool`, ЛИБО объявление `meta_pool` последним членом; плюс дренаж на пути исключения в цикле планировщика; плюс привести комментарий `CasGc.cpp:391` в соответствие.

## B2 — Два `LOGICAL_ERROR`-abort, достижимые сменой окружения (`claim()`, delete-marker) (подтверждено, P1) {#b2}

**Оба abort-пути живы на HEAD без изменений: `MountLeaseKeeper::claim()` бросает `LOGICAL_ERROR` на четырёх ветках (964/976/1017/1021), а GC-сайт delete-marker'а — на `Gc/CasGc.cpp:803`; отслеживается только второй.**

Что было заявлено (blocker 2): два пути, достижимых обычным изменением окружения, конструируют `LOGICAL_ERROR`, что в debug/ASan-сборках вызывает `abort()` на фоновом потоке — (2a) `MountLeaseKeeper::claim()`, (2b) единственный сайт удаления тела блоба в GC при `created_delete_marker`.

=== 2a — `MountLeaseKeeper::claim()` — ПОДТВЕРЖДЕНО, код не менялся ===

`Pool/CasServerRoot.cpp`, тело `MountLeaseKeeper::claim` (`:930-1029`) — номера строк совпадают с ревью почти буква в букву:
- `:964` — `throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount-lease: key '{}' is held by a foreign server ({}) — failing closed, never taking over", ...)` — чужой uuid;
- `:976` — `throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount-lease: key '{}' is held by a different writer_epoch ({} != ours {}) — superseded, failing closed ({})", ...)`;
- `:1017` — `... "was touched while adopting our own mount slot ({}) — failing closed"` (гонка adopt-overwrite);
- `:1021` — `... "vanished while adopting our own mount slot — failing closed"`.
Плюс ещё два в том же методе, которые ревью не перечислило: `:943` (`putIfAbsent` не Done) и `:954` (ключ исчез между head и get) — та же природа.
Ветка `gc_fenced` (`:988`) и одна ветка adopt-гонки (`:1011`) действительно бросают `MountFencedException` — то есть частичная работа по классам ошибок здесь была, но именно на «environment changed» ветках `LOGICAL_ERROR` остался.

Достижимость сохранилась: `Pool/CasPool.cpp:209` регистрирует `tryRemountOnce` как `remount_attempt`-колбэк фонового потока; `Pool::tryRemountOnce` (`:1007`) на `:1168` вызывает `mount_runtime.keeperStart()` → `SingleWriterSlot::doStart()` (`Pool/CasServerRoot.cpp:1299`) → `claim()`. То есть бросок происходит на фоновом потоке самопере-монтирования, ровно как описано.

Асимметрия с renewal-путём тоже сохранилась и теперь ЗАДОКУМЕНТИРОВАНА в самом коде: `onRenewMismatch` (`Pool/CasServerRoot.cpp:1070-1156`) содержит комментарий `:1070-1072` «below, each fail-closed and NONE constructing a `LOGICAL_ERROR`, which aborts debug/ASan» и бросает `ABORTED` (`:1101`, `:1114`, `:1145`) / `MountFencedException` (`:1083`) / `FILE_DOESNT_EXIST` (`:1158`). Более того, комментарий `:1136-1143` прямо признаёт незавершённость работы, но по ДРУГОМУ пути (release/terminate), перечисляя три `EXPECT_DEATH`-теста, которые пинят abort: `CasGcRound.OrphanManifestCursorSweepDeletesAndPersistsCursor`, `CasMountStartup.StaleSelfMountReclaimedAfterWait`, `CasPoolRemount.ForeignOwnerIsNeverTakenOver`. Про `claim()` в этом рассуждении нет ни слова — то есть исправление к нему не применили и не отклонили осознанно.

Пиннинг-тест жив и не изменён: `src/Disks/tests/gtest_cas_mount.cpp:728-737` — `MountLeaseKeeper k2(... epoch 8 ...); EXPECT_DEATH({ DB::abort_on_logical_error.store(true, ...); k2.start(); }, "held by a different writer_epoch");` (файл переехал из `.../ContentAddressed/tests/` в `src/Disks/tests/`, содержание то же).

Фиксящего коммита нет.

=== 2b — GC delete-marker — ПОДТВЕРЖДЕНО (форма кода), но приоритет спорен ===

`Gc/CasGc.cpp:802-806` (ревью указывало `:803`):
```
DeleteOutcome del = backend.deleteExact(layout.blobKey(entry.ref), entry.token);
if (del.created_delete_marker)
    throw Exception(ErrorCodes::LOGICAL_ERROR,
        "CAS gc: delete of blob {} created a delete marker — versioning is enabled "
        "on the pool (mis-provisioned; the capability probe must reject this)", blobIdOf(entry.ref));
```
Проба по-прежнему отбивает versioning только на входе: `Backend/CasProbe.cpp:217` (шаг 8, `NOT_IMPLEMENTED`), вызывается на каждом writable-монтировании (`Pool/CasPool.cpp:381` → `:457` → `:467`) и пропускается при `skip_access_check`. Включение versioning ПОСЛЕ монтирования по-прежнему приводит к `LOGICAL_ERROR` в раунде GC.

Сверка с 2031-triage CAS-029 (`docs/superpowers/cas/2031-triage.md:47`, `:1584`) — расхождений по ФАКТАМ нет, расхождение по оценке:
- CAS-029 подтверждает ровно тот же сайт («`LOGICAL_ERROR` после уже выполненного удаления — есть», `2031-triage.md` п.3) и добавляет то, чего в blocker 2 не было: `created_delete_marker` проверяется в 1 из 8 destructive-сайтов раунда (игнорируется на `Gc/CasGc.cpp:1193`, `:1243`, `:3461`, `:3563`, `:3569`, `Gc/CasNamespaceJanitor.cpp:111`, `Gc/CasOrphanManifestSweep.cpp:586`);
- CAS-029 ставит P3 и «не pre-release», обосновывая тем, что заявленной дыры (незащищённый versioned-бакет) нет — она закрыта обязательной поведенческой пробой, а остаток = неверный код ошибки + асимметрия детекции. Fable-ревью ставит blocker, потому что смотрит на ДРУГОЙ аспект: не «versioning проскочит», а «abort сервера в debug/ASan на фоновом потоке». Оба верны; тезисы не противоречат друг другу.

Мой вердикт по остатку 2b: приоритет CAS-029 (P3) занижен для sanitizer-полос, но выше P2 не поднимается — триггер требует внешнего действия оператора на живом пуле (`put-bucket-versioning`), чего в CI/соаке не происходит.

=== Итоговая оценка ===
P1 ставлю за 2a: это НЕ экзотика — «foreign uuid» и «different writer_epoch» достижимы пересозданием пула под тем же префиксом и обычной гонкой double-start (тот же класс условий, ради которого renewal-путь уже переписали на `ABORTED`), а срабатывает оно на фоновом потоке в debug/ASan-сборках — то есть ровно на полосах, которыми фича сертифицируется. 2b сам по себе — P3 и уже отслеживается.

Что осталось сделать: (а) переклассифицировать четыре (реально шесть) броска в `claim()` в `ABORTED`/`MountFencedException` по образцу `onRenewMismatch`, обновив `gtest_cas_mount.cpp:731` с `EXPECT_DEATH` на `EXPECT_THROW`; (б) отдельно решить судьбу release/terminate-пути (три пиннящих `EXPECT_DEATH`-теста, см. комментарий `CasServerRoot.cpp:1136-1143`) — это уже ruled decision, а не локальный фикс; (в) 2b — по плану анкора {#versioning-enabled-after-mount}.

## B3 — Ad hoc `disk(metadata_type=cas)` обходит привилегии `SYSTEM CAS` (подтверждено, P1) {#b3}

**Гейта нет: `registerContentAddressedMetadataStorage` по-прежнему не видит и не проверяет `custom_disk`, поэтому inline `disk(metadata_type='cas', …)` минтит полноценного члена пула в обход всех семи привилегий `SYSTEM CAS *`.**

Что было заявлено (blocker 3): фабрика метаданных строит полный CAS-store без какого-либо `custom_disk`-гейта; `ContentAddressedSettings` принимает `use_environment_credentials`; `DiskFromAST`'s `custom_local_disks_base_directory` не применяется к remote-дискам; диск кэшируется процессно; `gc_enabled` по умолчанию `true`. Итог — любой пользователь с `CREATE TABLE` минтит нового постоянного члена живого разделяемого пула.

Что на HEAD — все пять фактов подтверждаются, ни один не исправлен:

1. **Гейта в фабрике нет.** `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:217-244`, `registerContentAddressedMetadataStorage`: тело содержит только `checkSingleLocation(cluster)`, `takePointingTo`, `getObjectKeyCompatiblePrefix`, `settings.loadFromConfig`, `fs::create_directories` и `return std::make_shared<ContentAddressedMetadataStorage>(...)`. `grep -n custom_disk` по `MetadataStorageFactory.cpp` и `.h` — 0 попаданий: флаг до фабрики метаданных вообще не доходит (он живёт в `DiskFactory::Creator`, `src/Disks/DiskFactory.h:33`).

2. **Прецедент-фикс так и остался единственным в своём роде.** `src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:94-100` — тот самый отказ от `use_fake_transaction=true` для `MetadataStorageType::CAS`, реализованный по коду метаданных, а не по `custom_disk`. Сам колбэк регистрации диска `custom_disk` тоже не читает.

3. **`use_environment_credentials` по-прежнему в allowlist** — `ContentAddressedSettings.cpp:56`: `"endpoint", "access_key_id", "secret_access_key", "region", "use_environment_credentials", …`. Комментарий выше (`:25`, `:37`, `:41-43`) прямо описывает inline SQL `disk(...)` как штатную, покрытую тестами форму (в том числе «`DiskFromAST` for the inline SQL `disk(...)` form»). То есть это не забытая дыра, а задокументированный внутри кода путь.

4. **`DiskFromAST` не ограничивает remote-диски.** `src/Disks/DiskFromAST.cpp:97` создаёт диск с `/* custom_disk */true` и `markDiskAsCustom`; проверка `custom_local_disks_base_directory` на `:115-120` стоит под условием `if (!attach && !disk->isRemote() && disk->getName() != "backup")` — для S3-CAS не срабатывает никогда.

5. **Процессное кэширование и фоновые потоки.** `src/Interpreters/Context.cpp:6679-6693` `getOrCreateDisk` кладёт диск в `DiskSelector` под `storage_policies_mutex` и оставляет его там. `ContentAddressedSettings.cpp:73`: `DECLARE(Bool, gc_enabled, true, "Run the background GC scheduler on this disk", 0)` — то есть ad hoc диск сразу становится кандидатом в GC-лидеры.

6. **Привилегий по-прежнему ровно семь и все GLOBAL** (`src/Access/Common/AccessType.h:355-361`): `SYSTEM CAS GC RUN`, `GC REBUILD`, `DROP POOL MEMBER`, `FSCK`, `FORGET`, `GC STOP`, `GC START`. Ни одной привилегии на СОЗДАНИЕ/монтирование CAS-диска не появилось — асимметрия «удалить члена пула может только админ, а добавить — любой автор `CREATE TABLE`» сохраняется в полном виде.

Фиксящего коммита нет.

Сверка со смежной триажей: 2031-triage CAS-132 (`docs/superpowers/cas/2031-triage.md:5729`) касается этой темы одной фразой — «inline-`disk()` требует, чтобы пользователь САМ принёс бакет и креды» — и на этом основании закрывает СВОЮ находку (про утечку путей в текстах ошибок) как not-a-bug. Применительно к blocker 3 эта посылка неверна на HEAD: `use_environment_credentials` в allowlist (`ContentAddressedSettings.cpp:56`) означает ровно противоположное — пользователь может НЕ приносить креды и переиспользовать ambient IAM сервера. Так что CAS-132 не покрывает и не опровергает B3.

Что осталось: весь дефект. Уточнение к оценке серьёзности: злоупотребление требует, чтобы атакующий знал `server_root_id`/`pool_prefix` чужого пула и чтобы у сервера был ambient-доступ к тому бакету — то есть это не «любой пользователь читает чужие данные из коробки», а privilege escalation в конкретной (и распространённой) операционной конфигурации. P1 ставлю потому, что дешёвого post-release-исправления нет: как только ad hoc CAS-диски объявлены поддерживаемыми и покрыты тестами, запрет становится ломающим изменением. Минимальный вариант фикса — отклонять `custom_disk=true` для pool-joining metadata-типов по образцу `use_fake_transaction` (`RegisterDiskObjectStorage.cpp:97`), с явным опт-ином через настройку сервера для тестов; хотя бы — потребовать отдельный грант.

## B4 — Гонка concurrent `FETCH PARTITION`: тест выключен, трекера нет (B66a) (частично, P3) {#b4}

**Тест по-прежнему выключен и concurrent-варианта под CA нет, атомарная публикация ref не сделана — но заявление «B66a не отслеживается нигде» опровергнуто (это артефакт ветки поставки, где нет `docs/superpowers/`), а сам механизм в комментарии теста устарел и класс на самом деле local-only.**

Что было заявлено (blocker 4): тег `no-cas-storage` на `03350_alter_table_fetch_partition_thread_pool.sql` описывает torn read общего «detached» ref-объекта при параллельном fan-out FETCH; атомарная публикация pointer-объекта не реализована; `B66a` не встречается больше нигде в дереве — ни бэклога, ни issue, ни другого теста; «ничто в CI никогда не покраснеет».

Что на HEAD:

**1. Тест по-прежнему исключён, текст комментария не менялся.** `tests/queries/0_stateless/03350_alter_table_fetch_partition_thread_pool.sql:2` — тег `no-cas-storage` (переименован из `no-content-addressed-storage` коммитом `c4f0ba4184f`, 2026-08-03, чисто механически) с дословно тем же обоснованием, включая «…is a deferred backlog item (B66a); single-part FETCH works (01650 + 05002)». Единственный коммит, менявший этот файл после ревью, — тот самый переименовывающий тег. Concurrent-варианта под CA не появилось: `tests/queries/0_stateless/05002_cas_fetch_partition.sql` (58 строк) не содержит ни `concurrent`, ни `parallel`, ни `thread_pool` — это по-прежнему однопартовый FETCH.

**2. Атомарная публикация не сделана; механизм жив.** `ObjectStorageBackend::emuWrite` (`Backend/CasObjectStorageBackend.cpp:566-577`) по-прежнему пишет через `object_storage->writeObject(StoredObject(emuPath(key)), WriteMode::Rewrite, attrs)`, а `LocalObjectStorage::writeObject` (`src/Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.cpp:284-310`) открывает `WriteBufferFromFile` прямо на финальном ключе в режиме Rewrite — ни temp-файла, ни `rename`. То есть torn read на локальном бэкенде остаётся возможен.

**3. Утверждение «B66a нигде нет» — НЕ ПОДТВЕРДИЛОСЬ, и это методологический артефакт.** На HEAD `git grep B66a` даёт содержательные попадания, главное из них — `docs/superpowers/cas/BACKLOG/formats-and-storage.md:55`:
«**[B66a] concurrent-fetch torn read of a shared `detached` ref on local storage** — MINOR — `LocalObjectStorage` write is not atomic… Safe on S3 (atomic PUT). Freeze dodged this class by design (one ref per frozen part, no shared container by design); the residual case is concurrent writers of one `detached/<part>` name. Mechanism is closed by the temp-file+rename item above; until then racy multi-writer on local/NFS stays documented-unsafe.»
Запись не новая: она пришла ещё коммитом `45a6c8ee2b6` («docs(cas): backlog grooming», 2026-07-13) и была переразложена по тематическим файлам коммитом `0f266066bef` (2026-08-04), то есть существовала ДО ревью. Причина расхождения проверяема: `git ls-tree 056488b47a0 docs/superpowers` пусто — на ветке поставки, по которой шло ревью, внутреннего каталога `docs/superpowers/` просто нет. Так что «нет тракера в 573-файловом диффе» было буквально верно и одновременно ложно как вывод о том, отслеживается ли пункт.

**4. Заявленный масштаб воздействия завышен, а механизм в комментарии теста устарел.**
- «Safe on S3 (atomic PUT)» из бэклога подтверждается кодом: класс относится только к `Emulated`/local-бэкенду; на Native/S3 публикация ref — атомарный PUT. Формулировка ревью «Ordinary multi-part `ALTER TABLE … FETCH PARTITION` on CAS (routine replication/rebalancing) surfaces as user-visible spurious errors» верна лишь для local/NFS-пула, который сам по себе документирован как «racy multi-writer … documented-unsafe» (`formats-and-storage.md:33-38`: «Nothing in this section affects S3/GCS production pools»).
- «SHARED "detached" ref object» из комментария теста больше не описывает код: detached-части живут как ОТДЕЛЬНЫЕ ref'ы `detached/<part>` внутри собственного namespace таблицы — `Parts/PartPathParser.h:43-49` (`kDetachedRefPrefix`, «One namespace per table… No parallel detached namespace exists anymore»), это работа B181 (`a0493e26d0f`, `0e40a1f0fc3`, `d74c12d9576`, `fa4c7aad733`), тоже сделанная ДО головы ревью. Fan-out на 100 РАЗНЫХ частей пишет 100 разных ref-ключей, а не один общий объект. Остаточный случай, как и говорит бэклог, — два конкурентных писателя ОДНОГО имени `detached/<part>`, чего этот тест не создаёт. Ни ревью, ни триаж этого не заметили, потому что оба читали комментарий теста как описание текущего кода.

**Что осталось (в порядке убывания ценности):**
- (а) перепроверить, ЗАСЛУЖЕН ли ещё тег: прогнать `03350` под CA-полосой и посмотреть, падает ли он вообще — если премисса комментария устарела вместе с B181, тег снимается бесплатно, и дыра в CI закрывается без единой строчки продуктового кода. Это самое дешёвое действие и его стоит сделать первым;
- (б) если падает — привести комментарий в соответствие с реальным механизмом (общий ref-лог namespace'а на неатомарном локальном бэкенде, а не «shared detached ref»);
- (в) продуктовый фикс — пункт `[disk-error-audit]` (temp-file + `rename` в `emuWrite`/`LocalObjectStorage`), который бэклог прямо называет закрывающим механизм B66a.

P3 и не pre-release: на S3/GCS-пулах (единственная production-посадка) класс не воспроизводится, а local/NFS уже задокументирован как небезопасный для multi-writer. Реальная цена сегодня — не риск для данных, а слепое пятно в CI плюс, возможно, лишний тег на тесте, который бы уже проходил.

## M5 — `ContentAddressedTransaction` стейджит записи манифеста за O(F²) (подтверждено, P3) {#m5}

**Форма подтверждена дословно — staging по-прежнему вектор без индекса по пути, все восемь upsert'ов и четыре read-your-writes скана на месте; но цена переоценена (миллионы сравнений коротких строк против одного PUT на файл), одна действительно квадратичная часть (dedupe в `uploadPendingBlobs`) уже закрыта.**

Заявлено (обзор, п.5): `PartStaging::entries` — `std::vector<Cas::ManifestEntry>`; каждый staged-файл делает `std::erase_if(entries, path==)` + `push_back`, а read-your-writes методы (`findStagedEntry`, `hasInFlightDirectory`, `listInFlightDirectory`) линейно перечитывают тот же вектор. Итог — O(F²) на парт, предложено перейти на `unordered_map<String, ManifestEntry>` и материализовать/сортировать вектор один раз на публикации.

На HEAD (`d49999a42fb`) — без изменений по существу:
- `ContentAddressedTransaction.h:139` — `std::vector<Cas::ManifestEntry> entries;   /// staged manifest entries (uploads + adoptions)`. Контейнер тот же.
- Upsert'ы `erase_if` + `push_back` на восьми площадках: `ContentAddressedTransaction.cpp:718-719` (stage blob), `:951-952` (inline), `:1181-1182` и `:1202-1203` (`createHardLink`), `:1353-1354` (`moveDirectory` re-key), `:1526-1527` (`moveFile`), `:1551` (`replaceFile`), `:1593` (`unlinkFile`).
- Линейные пробы: `findStagedEntry` — `:543-552` (`std::find_if` по `it->second.entries`), `hasInFlightDirectory` — `:648-668` (цикл по `entries` с `starts_with(prefix)`), `listInFlightDirectory` — `:670+` (полный проход + `std::set` имён).
- Вложенный скан в `moveDirectory` (`:1345-1354`) сохраняется: приёмник растёт внутри того же цикла, так что оговорка комментария о «свежесозданном приёмнике» линейности не даёт.

Фиксящего коммита нет. `git log 056488b47a0..HEAD -- ContentAddressedTransaction.cpp` (37 коммитов, включая writepath stage-1 T5 `fff1c21989d`, TXN-pipeline `04ee9638de9`/`a97100ca48f`) не трогает структуру staging: stage-1 бил по параллелизму загрузки блобов, а не по CPU стейджинга.

Что уже закрыто из этого класса: дублирующая проверка членства в `uploadPendingBlobs` — теперь `std::unordered_set<Cas::BlobRef, Cas::BlobRefHash> referenced_hashes` (`ContentAddressedTransaction.cpp:261-264`), а схлопывание дублей ушло в группировку `fanOutBlobUploads` (комментарий `:266-269`). Это единственный настоящий quadratic-по-членству кусок, и он ушёл.

Что осталось и почему P3: чисто предупредительный CPU-долг. Сравнения — `std::string ==` по коротким именам файлов (сначала длина, потом `memcmp`), `ManifestEntry` при промахе не копируется. Для очень широкого парта с проекциями и вторичными индексами (~3000 файлов) это единицы миллионов сравнений — единицы миллисекунд — против 3000 локальных временных файлов и 3000 blob-PUT на том же пути. Квадратичности по БАЙТАМ нет, между партами тоже нет (каждый `parts`-элемент сканируется отдельно, `ContentAddressedTransaction.h:157`). Фикс (индекс `path -> index` рядом с `entries` либо `std::map` по пути с отказом от пересортировки в `encodePartManifest`, плюс устранение вложенного скана в `moveDirectory`) стоит делать только когда staging проявится на профиле.

## M6 — Blob-upload pool: сырая ссылка убегает из-под лока; `clickhouse-local` рушит порядок teardown (подтверждено, P2) {#m6}

**Обе половины на месте — `blobUploadPool()` по-прежнему отдаёт сырую `ThreadPool &` из-под уже отпущенного `pool_mutex`, а `clickhouse-local` по-прежнему гасит пул ДО `global_context->shutdown()`, в отличие от `clickhouse-server` и `clickhouse-disks`.**

Заявлено (обзор, п.6): `blobUploadPool()` возвращает `*pool_instance` после выхода из-под `pool_mutex`; `shutdownBlobUploadPool()` = `pool_instance.reset()`; ссылка удерживается на всю раздачу в `ContentAddressedTransaction.cpp:309`; `LocalServer.cpp` вызывает shutdown пула ПЕРЕД `global_context->shutdown()` — обратный порядок относительно `Server.cpp` и `DisksApp.cpp`. Предложено: перенести вызов в `LocalServer::cleanup()` после `global_context->shutdown()`, а в перспективе отдавать `shared_ptr<ThreadPool>` (или счётчик использований, дренируемый на shutdown).

На HEAD (`d49999a42fb`) — без изменений:
- `Pool/CasBlobUploadPool.cpp:52-59` — `ThreadPool & blobUploadPool() { std::lock_guard lock(pool_mutex); if (!pool_instance) throw …; return *pool_instance; }`: `lock_guard` умирает на выходе, ссылка живёт дальше. `:61-65` — `shutdownBlobUploadPool()` это `pool_instance.reset()` под тем же мьютексом.
- Единственный продовый потребитель — `ContentAddressedTransaction.cpp:309`: `Cas::fanOutBlobUploads(*st.build, requests, Cas::blobUploadPool());`, то есть ссылка живёт весь submit-and-join.
- Контракт в заголовке остаётся словесным: `Pool/CasBlobUploadPool.h:29-31` — «The returned reference is only valid while the pool stays initialized: callers must not race this against `shutdownBlobUploadPool`». Ни assert'а, ни счётчика.
- Порядок остановки: `programs/local/LocalServer.cpp:918` `DB::Cas::shutdownBlobUploadPool();` стоит ДО `:920-922` `if (global_context) { global_context->shutdown(); … }`. У соседей наоборот: `programs/server/Server.cpp:1513` — внутри `SCOPE_EXIT_SAFE`, который отрабатывает в самом конце `main`, то есть после `:1604 global_context->shutdown()`; `programs/disks/DisksApp.cpp:673` — в `SCOPE_EXIT_SAFE` в `mainEntryClickHouseDisks`, то есть после `~DisksApp`, который на `:631` делает `global_context->shutdown()`.

Фиксящего коммита нет; более того, текущее размещение — сознательное. Место в `LocalServer` появилось в `510e9e6652f` («ca: stage1 T5 fix — blob upload pool wiring for clickhouse-local and clickhouse-disks»), а формулировка контракта в заголовке — в `25b37e36175` («stage1 T1 adjudication — shutdown ordering follows the IO-pools convention; getter lifetime contract»), где вердикт был «placement stands», обоснование: «T5's submit-and-join discipline keeps every pool task inside its transaction scope». Оба коммита старше ревью (23-24 июля против 5 августа), так что за прошедшие недели тут не менялось ничего.

Остаточный риск (почему это всё-таки дефект, а не закрытая позиция): комментарий на `LocalServer.cpp:915-917` оправдывает ранний shutdown тем, что задачи ссылаются на контекст — но `MergeTreeData` поднимает общие фоновые исполнители и под `clickhouse-local`, и до `global_context->shutdown()` фоновый мёрж может стоять внутри `fanOutBlobUploads`, держа ссылку на пул, который `pool_instance.reset()` уже разрушает. Исход — либо UAF, либо (если гонка легла иначе) `LOGICAL_ERROR` из `blobUploadPool()` на пути мёржа. Ни один из двух порядков не безопасен, пока контракт держится на дисциплине вызова; настоящее закрытие — предложенный ревью `shared_ptr`/счётчик использований, дренируемый в `shutdownBlobUploadPool`, после чего порядок перестаёт быть значимым.

P2, не P1: путь достижим только в `clickhouse-local` (не в сервере) и только если фоновый мёрж живёт в момент выхода процесса; последствие — падение на завершении CLI, не порча и не потеря данных.

## M7 — Инлайновые CAS-диски не сносятся на `DROP TABLE` (утечка lease + потоков) (частично, P2) {#m7}

**Утечка сама по себе на HEAD осталась (диск кэшируется в реестре навсегда, teardown'а на `DROP TABLE` нет), но её опасная половина закрыта раундом rev.8 — фоновые нити терминального пула сами выходят, а обращения к нулевому пулу бросают вместо abort'а; остаток осознанно отложен и затрекан.**

Заявлено (обзор, п.7): `~DiskObjectStorage` только логирует, а `shutdown()` (который останавливает GC-планировщик и keeper mount-lease) вызывается лишь из `Context`-шатдауна или при удалении диска из конфига; `DiskFromAST::getOrCreateCustomDisk` teardown'а по `DROP TABLE` не имеет, поэтому таблица с инлайновым `disk(metadata_type=cas, …)` после дропа оставляет живой диск с нитями, держащими mount lease, до конца процесса. Предложено: либо провод last-detach `shutdown()`/`forgetDisk()`, либо отказ от инлайновых CAS-дисков (что закрыло бы и блокер 3).

На HEAD (`d49999a42fb`) механика описана верно:
- `src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:232-235` — `DiskObjectStorage::~DiskObjectStorage() { LOG_INFO(log, "Destroying disk {}", name); }`, ничего не останавливает; вся остановка в `:504-513` `shutdown()` (`blob_killer->shutdown()`, `blob_copier->shutdown()`, `metadata_storage->shutdown()`, `object_storages->takePointingTo(location)->shutdown()`).
- `IDisk::shutdown` документирован ровно как «Invoked when Global Context is shutdown» (`src/Disks/IDisk.h:489-490`), и единственные продовые вызовы — `src/Interpreters/Context.cpp:1060-1074` (обход `merge_tree_disk_selector->getDisksMap()` на шатдауне глобального контекста + временные тома) и `DiskSelector::shutdown()`/error-path (`src/Disks/DiskSelector.cpp:141`, `:254-259`).
- Инлайновый диск попадает в тот же реестр навсегда: `DiskFromAST.cpp:97` создаёт его с `/* custom_disk */true`, а `Context::getOrCreateDisk` (`src/Interpreters/Context.cpp:6679-6693`) кладёт его в `DiskSelector` через `addToDiskMap` и больше никогда не удаляет. Ни `DROP TABLE`, ни detach на диск не смотрят; CAS-специфичного отказа для `custom_disk` в `DiskFactory`/`DiskFromAST` тоже нет (это отдельный блокер B3).

Что закрыл раунд rev.8 (та половина, что делала утечку опасной): фоновые нити теперь сами выходят на терминальном пуле, а не бьются об удалённый префикс. `Gc/CasGcScheduler.cpp:365-378` — heartbeat-цикл проверяет `store->isVanished() || store->vanishedIntentPublished() || store->lifecycle() == Cas::PoolLifecycle::IdentityLost` и возвращается; тот же self-exit в `loop()`. По записи BACKLOG цели G1-G5 раунда закрыты (изоляция, throw-not-abort, self-exit GC на `Vanished`/`IdentityLost`, корректность в общем коде, FSCK-on-running), то есть memory-пункт «CAS disk-lifecycle-leak mount abort» (abort фоновой нити на удалённом каталоге пула) более не воспроизводится в этой форме — вместо abort'а фиксируется fail-loud `INVALID_STATE` (residual (b) там же).

Что осталось: сам eject. Цитата раздела `BACKLOG/mounts-and-lifecycle.md:31-35`: «NOT resolved (deliberately deferred): the underlying disk-lifecycle-leak proper — a CA disk is still cached forever in the disk registry (`Context::getOrCreateDisk`) with no teardown/eject on `DROP TABLE`… The Dormant/UNMOUNT/MOUNT reuse machinery that pursued this was rolled back (spec rev.8 §9); `FORGET` is the node-local decommission story. Full eject-on-`DROP` is future work». То есть предполагавшегося в задании SQL-глагола `UNMOUNT` в коде НЕТ (в `ASTSystemQuery.h:154-160` только `CAS_GC_RUN/GC_REBUILD/DROP_POOL_MEMBER/FSCK/FORGET/GC_STOP/GC_START`), а node-local decommission делает `SYSTEM CAS FORGET`.

Цена утечки измерена (`BACKLOG.md:481-484`): четыре диска завершившихся тестов продолжали крутить GC-раунды с частотой ~1 Гц до конца прогона, `readdir`/`lstat` дали 11.7% профиля — «leaked 1 Hz schedulers from completed tests kept scanning for the rest of the run».

P2, не P1: последствие — утечка нитей/аренды монтирования и фоновый трафик до перезапуска процесса, без потери и порчи данных; отказ громкий; продовая конфигурация с дисками из `config.xml` не задета (там диск и должен жить до шатдауна). Инлайновый CAS-`disk()` до релиза правильнее закрыть по линии B3 (отказ от ad hoc CAS-дисков), что попутно снимает и этот сценарий.

## M8 — Сетевой деструктор `Pool` может выполняться под `pointer_mutex` (подтверждено, P3) {#m8}

**Форма подтверждена дословно: в `shutdown()` `part_access.reset()` и `cas_store.reset()` по-прежнему выполняются ПОД `pointer_mutex`, а при отключённом GC именно там срабатывает сетевой деструктор `~Pool` (дренаж ref-полос + прощальная запись), блокируя всех читателей снимка.**

Заявлено (обзор, п.8): в `ContentAddressedMetadataStorage.cpp` (тогдашние `:887-909`) сбросы `part_access`/`cas_store` находятся внутри области `pointer_mutex`, тогда как `old_scheduler` корректно вынесен наружу; `~Pool` (`CasPool.cpp:861-889`) делает `drainRefLanesForShutdown(...)` — сетевые секунды — и `finishTeardown()` с долговечной записью. Когда `gc_scheduler` пуст (GC выключен или read-only-монтирование), последняя сильная ссылка — именно `cas_store`, и вся эта сетевая работа идёт под «кратким снимочным» локом. Предложено: снять `cas_store`/`part_access` в локальные переменные, отпустить `pointer_mutex`, сбрасывать вне лока — зеркально `old_scheduler`.

На HEAD (`d49999a42fb`) всё на месте:
- `ContentAddressedMetadataStorage.cpp:921-941` — `shutdown()`: `std::lock_guard round_lock(gc_scheduler_mutex);` … затем блок `{ std::lock_guard ptr_lock(pointer_mutex); old_scheduler = std::move(gc_scheduler); gc_scheduler.reset(); part_access.reset(); cas_store.reset(); }`, и только `old_scheduler->stop()` вынесен наружу с комментарием «Runs outside pointer_mutex … but still inside round_lock». Про сбросы `part_access`/`cas_store` комментария нет — их сетевую стоимость никто не учёл.
- `Pool/CasPool.cpp:872-900` — `Pool::~Pool()`: `mount_runtime.stopRemountThread()`, затем `ref_ledger.drainRefLanesForShutdown(config.cas_request_budget.attempt_timeout_ms + config.cas_request_budget.lease_safety_margin_ms)` и `mount_runtime.finishTeardown(drained)` (терминальная операция keeper'а — долговечная запись). Дефолты бюджета — `Backend/CasRequestControl.h:151` `attempt_timeout_ms = 5000` и `:177` `lease_safety_margin_ms = 2000`, то есть дренаж ограничен сверху ~7 с, плюс сетевая терминальная запись.
- Маскировка планировщиком — действительно случайность refcount'а, а не инвариант: `ContentAddressedMetadataStorage.cpp:882-889` создаёт `CasGcScheduler` только `if (context && gc_enabled && !read_only)`, а сам планировщик держит собственный `Cas::PoolPtr` (`Gc/CasGcScheduler.h:179 const Cas::PoolPtr store;`, комментарий в `:875-876`). При выключенном GC / read-only монтировании / null-контексте ссылок больше нет. Замечу дополнительно, чего в ревью не было: `part_access` тоже держит `PoolPtr` (`Parts/PartFolderAccess.h:365`), так что последняя ссылка вообще может умереть на строке `part_access.reset()` — тоже под локом.
- Кого это блокирует: `poolAccess()` — `:1100-1120` — берёт `pointer_mutex` ради копии двух `shared_ptr`; через него идут `store()` (`:1133`) и `partAccess()` (`:1138`), то есть весь store-класс, а также интроспекция `system.cas_mounts` (`gcHealth`/`lifecycleSnapshot` документированы как читаемые в ЛЮБОМ состоянии, `:134-136`).

Фиксящего коммита нет. `452d17af42f` («partAccess returns a shared-ownership snapshot; gcHealth no longer blocks behind a GC round») занимался ровно соседней проблемой — вынес читателей из-под `gc_scheduler_mutex`, но `pointer_mutex`-сбросы не тронул. `git log 056488b47a0..HEAD` по этому файлу ничего про порядок сбросов не содержит.

P3, не выше: путь достижим только на самом шатдауне диска/сервера (`IDisk::shutdown` вызывается из `Context.cpp:1060-1074` при шатдауне глобального контекста), ожидание ограничено сверху ~7 секундами бюджета попытки + терминальная запись, порчи и потери данных нет — последствие в том, что на несколько секунд встают все читатели снимка, включая интроспекцию, которая специально проектировалась как «никогда не блокирующаяся». Правильно закрывается ровно предложенным ревью способом: снять оба указателя в локальные `shared_ptr` внутри лока, `reset()` членов там же, а уничтожение локальных — после выхода из области `pointer_mutex` (одна строка порядка, зеркало уже существующей обработки `old_scheduler`). Смежно: `forgetDisk` (`:960-980`) уже делает это правильно — там `pool`/`scheduler` копируются под локом, а весь протокол с join'ами идёт вне него.

## M9 — GCS `gcp_oauth` переписывает ВСЕ запросы/ответы, не только условные записи (исправлено, P3) {#m9}

**Закрыто коммитом `faab6678d8f` (21.08): клиент-широкий флаг `gcs_conditional_dialect` удалён, вся GCS-адаптация запроса и подмена `ETag` в ответе теперь применяются только к запросам, помеченным CAS как `NativeConditional`; остаток — прогон на живом GCS (Task 9) и релиз-нота про новую строгость `gcs_hmac`.**

Заявлено (обзор, п.9): `Client.cpp:1310-1314` выставлял `gcs_conditional_dialect = true` для всего клиента, как только `http_client == "gcp_oauth"`; `GCSConditionalDialect.cpp:36-84` безусловно срезал `x-amz-date`/`content-sha256`/`security-token`/`api-version` и переименовывал `x-amz-*` → `x-goog-*`; `PocoHTTPClient.cpp:751-777` перетирал `ETag` ответа значением `x-goog-generation`. То есть существующие НЕ-CAS инсталляции на `gcp_oauth` (фича из v26.2, PR #96975) молча получали смену формата на проводе и семантики `ETag` при апгрейде. Предложено гейтить переписывание на факт условного заголовка (или на CAS-диск), как минимум — релиз-нота.

На HEAD (`d49999a42fb`) находка закрыта, и закрыта именно так, как предлагалось (гейт по запросу, а не по клиенту):
- Флага больше нет: `git log -S "gcs_conditional_dialect"` → `faab6678d8f` «Isolate GCS generation adaptation to explicitly marked CAS requests» (2026-08-21, в HEAD). Из его сообщения: «`Client::usesGcsConditionalDialect` and the `gcs_conditional_dialect` flag are removed»; grep по `src/` даёт ноль вхождений.
- Гейт по запросу: `src/IO/S3/PocoHTTPClient.cpp:757-761` — адаптация ответа только `if (isNativeConditionalRequest(request)) applyGcsConditionalDialectToResponse(...)`; `:909-915` (OAuth-путь) — «A `Default` request keeps pre-CAS upstream behaviour: the Bearer token replaces `Authorization` and every other SDK header is left alone», адаптация и `prepareGcsRequestForOAuthAuthentication` только под тем же `isNativeConditionalRequest`; аналогично `:1021-1022`.
- Носитель признака типизирован: `9601ac360f0` («Add typed `NativeConditional` request state» — `ObjectStorageRequestMode`, `ExtendedRequest::setNativeConditional`, пере-вывод бита на каждой SDK-попытке, т.к. ретрай/редирект строит новый HTTP-запрос), затем `5d7f26274cb` (HEAD/DELETE), `9b887ac8886` (записи), `faab6678d8f` (атомарное переключение адаптера).
- То, что раньше было клиент-широким флагом, теперь выведено из конфигурации только как КАПАБИЛИТИ, без изменения трафика: `Client.cpp:987-996` — `httpClientImpliesGcsGenerationDialect` / `Client::supportsGcsNativeConditionalRequests`, и комментарий `Client.h:263-265` явно говорит «independent of whether any given request opts in».
- Контракт зафиксирован в заголовке адаптера `src/IO/S3/GCSConditionalDialect.h:11-13` («ONLY for a request marked `NativeConditional`, so ordinary traffic through the same client keeps upstream AWS semantics») и `:42-47` («A `Default` response is never passed here and so keeps its upstream ETag and headers byte-for-byte»). Есть покрытие: `src/IO/S3/tests/gtest_gcs_conditional_dialect.cpp` (+375 строк в фикс-коммите), `gtest_aws_s3_client.cpp`, `gtest_goog4_signer.cpp`, `gtest_cas_probe.cpp`; отдельно `8562e4c1690` «Pin non-CAS GCS authentication behavior».
- Смежно закрыт риск смены диалекта на лету: `576e5511c22` «Refuse a token-dialect flip when a content-addressed disk reloads» + `2ca5677d588`.

Что осталось (почему P3, а не «—»):
1. Живой прогон. План `docs/superpowers/plans/2026-08-20-cas-gcs-request-isolation.md` содержит 9 задач; в коде видны 1-6, а задачи 7-9 (изоляция `ETag`/ключа кеша, валидация обоих нативных клиентов на живом GCS, финальная верификация и аудит форк-поверхности) как отдельные коммиты не прослеживаются, чекбоксы в плане не проставлены (85 незакрытых, 0 закрытых). BACKLOG подтверждает, что гейт ещё считается открытым: `formats-and-storage.md:26-27` — «must follow completion of the current GCS Task 9 gate», и там же `[GCS production-grade follow-ups]` держит «`gcp_oauth` dialect probe validation against live GCS (ADC creds)».
2. Новая пользовательская строгость, которую всё ещё стоит релиз-нотить (ровно в духе исходного «at minimum release-note it»): на `gcs_hmac` подпись GOOG4 идёт для КАЖДОГО запроса и теперь требует явной диспозиции для любого `x-amz-*` (`GCSConditionalDialect.h:32-40`), поэтому server-side encryption и пользовательские `x-amz-*`-заголовки диска отклоняются с `BAD_ARGUMENTS` вместо молчаливого переименования — это описано в `docs/en/antalya/cas/architecture/backend.md`, но в changelog-формулировке ещё нуждается.
3. Побочное следствие, зафиксированное в комментарии (`GCSConditionalDialect.h:48-51`): атрибуты CAS-объекта читаются только помеченным запросом — `Default`-чтение метаданных CAS-объекта отдаёт пустую карту, а не ошибку. Осознанно и задокументировано.

## M10 — Disk-transaction contract трогает 16 общих файлов MergeTree — нужен динамический non-CAS прогон (частично, P2) {#m10}

**Статическая половина на HEAD держится (все ветки по-прежнему дефолтно выключены для не-CAS), частичная централизация правила была уже в обозреваемом дереве и с тех пор не расширялась (~10 мест по-прежнему выводят правило заново, `PartTransactionScope`/debug-ассерта нет), а запрошенное динамическое доказательство «нет дельты против базы» на не-CAS так нигде и не зафиксировано — хотя апстримные (не-CAS) полосы CI на ветке сохранены и планируются каждым workflow.**

Заявлено (обзор, п.10): коммит `93d545b7446` «Disk-transaction contract: one logical part = one transaction» переписывает 16 общих (не-CAS) файлов MergeTree; статически каждая точка ветвления корректно дефолтится для не-CAS, но это самый высокий blast radius для существующих пользователей, и чтение кода не заменяет прогон. Плюс архитектурная претензия: правило «одна логическая часть = одна транзакция» — конвенция, переизобретаемая примерно в 10 независимых местах. Предложено: (а) подтвердить полное не-CAS покрытие stateless/stress (проекции+мутации, MOVE/REPLACE/ATTACH PARTITION, FREEZE, BACKUP/RESTORE на plain local и plain S3) без дельты против базы; (б) рассмотреть централизацию через один хелпер `PartTransactionScope` + debug-ассерт.

Коммит на месте: `git show 93d545b7446` (Aug 5 2026, «Projection sub-parts ride the parent whole-part transaction … read-your-writes … becomes part of the IDiskTransaction contract»), 16 файлов, из них общие MergeTree/Disks.

### (1) Статическая половина — на HEAD держится

Дефолты не тронуты:
- `src/Storages/MergeTree/IDataPartStorage.h:196` — `virtual bool isContentAddressed() const { return false; }`;
- `src/Disks/IDiskTransaction.h:145` — `virtual std::optional<StoredObjects> tryGetInFlightStorageObjects(...) const { return {}; }`, `:148` — `tryGetInFlightFileSize(...) { return {}; }`;
- `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.h:135` — ветка `if (metadata_storage->transactionIsStagingOverlay())` (для не-CAS метахранилищ ложь, дальше прежний путь).

Ключевые «безусловные» добавления по-прежнему за `hasActiveTransaction()`, т.е. no-op там, где транзакция не открыта:
- `src/Storages/MergeTree/MergeTreeData.cpp:9372` (renameParts-путь `Transaction`) и `:9394` (`Transaction::commit`) — `if (part->getDataPartStorage().hasActiveTransaction()) part->getDataPartStorage().commitTransaction();`;
- `:8737` — тот же гейт в OPTIMIZE DRY RUN;
- `:5967-5969` — «пустое покрытие» дополнительно сужено конъюнкцией `isContentAddressed() && hasActiveTransaction()`, с комментарием, привязывающим это к TXN-ONE-PIPELINE.

CA-специфичные ветки в общих файлах явно помечены и гейтятся по диску: `DataPartStorageOnDiskBase.cpp:424` (BACKUP через временные hardlink'и на CA — `SUPPORT_IS_DISABLED`), `:544` (`!params.external_transaction && disk->isContentAddressed()` — клон целиком в одной транзакции), `:745` (CA-приёмник в `clonePart`, ветка L2 MOVE-to-CA), `IMergeTreeDataPart.cpp:1435` (`use_parent_transaction = !is_temp_projection || isContentAddressed()`), `MergeTreeDeduplicationLog.cpp:114` (`type != Plain && type != CAS`). Ни одной незагейченной ветки при перепроверке не найдено — вывод обзора подтверждается на HEAD.

### (2) Централизация правила — частично, и НЕ после обзора

Централизация одна и она уже была в обозреваемом дереве: `73b03c9a1e4` «cas: encapsulate whole-part-transaction rule on borrowed projection storage» — «removing the 6 duplicated `if (!isContentAddressed()) begin/commitTransaction()` guards across MergeTask, MutateTask, MergeProjectionPartsTask and MergeTreeDataWriter», плюс `src/Storages/MergeTree/tests/gtest_projection_borrowed_transaction.cpp`. На HEAD это `DataPartStorageOnDiskFull.cpp:398-420` (`beginTransaction`/`commitTransaction` — `if (has_shared_transaction) return;`, комментарий `:401-404` прямо говорит «This centralizes the rule the 6 merge/mutate call sites used to duplicate»). Проверено, что то же самое есть и в дереве обзора: `git grep -c has_shared_transaction 056488b47a0 -- src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp` → 2. То есть обзор считал ~10 оставшихся мест УЖЕ после этой централизации.

С 2026-08-05 в этой части не изменилось ничего: `git log 056488b47a0..HEAD -- src/Storages/MergeTree/ src/Disks/DiskObjectStorage/` содержит только GCS-изоляцию, shadow-namespace `FREEZE` и merge апстрима — ни одного коммита про транзакционный контракт. `PartTransactionScope` в дереве отсутствует (`grep -rn PartTransactionScope src/` → пусто), debug-ассерта на правило нет. Правило по-прежнему выводится заново примерно в десятке мест: `MergeTreeData.cpp` (7 совпадений `isContentAddressed()/MetadataStorageType::CAS/hasActiveTransaction`), `DataPartStorageOnDiskBase.cpp` (6), по одному в `MergeTask.cpp`, `IMergeTreeDataPart.cpp`, `MergeTreeDeduplicationLog.cpp`, `MergeTreeDataWriter.cpp`, `MutatePlainMergeTreeTask.cpp`, `MutateFromLogEntryTask.cpp`, `DataPartStorageOnDiskFull.cpp`, два в `DataPartsExchange.cpp`. Риск ровно тот, что описан: новый путь записи файлов, забывший правило, для shared-transaction случая падает молча.

### (3) Динамическое доказательство на не-CAS — не найдено

Что ЕСТЬ:
- Апстримные полосы в CI на ветке сохранены и планируются: `ci/defs/altinity_jobs.py:126-131` — `functional_tests_jobs = [*JobConfigs.functional_tests_jobs, *cas_functional_tests_jobs]`, т.е. CAS-полосы ДОБАВЛЕНЫ к апстримным, а не заменяют их; CAS-полосы отдельны и параметризованы `cas storage` / `cas s3 storage` (`:74-121`). Значит механизм для не-CAS-прогона существует и работает на каждом PR ветки (есть и след разбора такого прогона — RCA CI PR #2073, ссылки в `docs/superpowers/cas/BACKLOG/gc.md:58`).
- Гейтинг тестов по CA-дефолту жив: `tests/clickhouse-test:1355` (`CAS_STORAGE = "cas-storage"`), `:2883` (тег `no-cas-storage`); заметим, тег переименован — старого `no-content-addressed-storage` в `tests/` больше нет ни одного (`git grep -l` → 0), тогда как `BACKLOG/testing-and-ci.md:25` всё ещё говорит про «remaining `no-content-addressed-storage` tests».

Чего НЕТ: ни одной записи с вердиктом «не-CAS прогон, дельты против базы нет». Единственные зафиксированные полные прогоны stateless — это ПРОТИВОПОЛОЖНОЕ направление (не-CA тесты под CA-дефолтным диском) и оба сами открыты: `BACKLOG/testing-and-ci.md:44` **[non-ca-stateless-fast-fails]** («TODO: re-run the CA-default stateless on a QUIET box (or real CI)») и `:45` **[full-ca-default-stateless-quiet-box]** («40 non-CA fails, dominated by CONTENTION TIMEOUTS … TODO: run the full CA-default stateless on real CI»). Ни один из них не отвечает на вопрос п.10, потому что там дефолтный диск — CAS. Прогонов по конкретному op-mix из «Fix» (проекции+мутации, MOVE/REPLACE/ATTACH PARTITION, FREEZE, BACKUP/RESTORE на plain local и plain S3) со сравнением против базы в `docs/superpowers/cas/` нет вообще.

### Что осталось
1. (главное) Зафиксировать не-CAS доказательство: прогон апстримных stateless+stress+integration полос ветки против базы `antalya-26.6` с явной записью «дельты нет», отдельно выделив BACKUP/RESTORE, FREEZE и партишен-операции на plain local и plain S3. Дёшево (полосы уже планируются), но результат нигде не записан — а именно записи и требовал п.10.
2. (желательно) Централизация: `PartTransactionScope` + debug-ассерт «файл части пишется под транзакцией части». Сейчас закрыт только projection-borrowed кусок.
3. (гигиена) Устаревшее имя тега в `BACKLOG/testing-and-ci.md:25` (`no-content-addressed-storage` → `no-cas-storage`).

P2, не до релиза: доказательство ценно, но статическая перепроверка на HEAD снова не нашла ни одной незагейченной ветки, а апстримные полосы CI на ветке не отключены — то есть регрессия, если бы она была, попадала бы в обычный отчёт CI, а не оставалась бы невидимой.

## M11 — Кросс-дисковый `MOVE PARTITION` (CAS ↔ non-CAS) достижим, но помечен непроверенным (частично, P2) {#m11}

**Самопризнание «cross-disk не проверено» на HEAD стоит дословно и `MOVE_PARTITION` из CA-allowlist не убран, но ландшафт с 05.08 изменился асимметрично: направление В CA реализовано (L1+L2), закрыто в бэклоге и прогнано сценариями S36/S37, а направление ИЗ CA осталось без гарда — и именно там 2031-триаж нашёл реальный дефект CAS-020 (серверная копия копирует байты конверта, спасает только громкий отказ на inline-файлах).**

Заявлено (обзор, п.11): в `MergeTreeData.cpp` `checkAlterPartitionIsPossible`, ветка `MetadataStorageType::CAS`, allowlist пропускает `MOVE_PARTITION`, а комментарий рядом сам говорит, что проверен только одно-дисковый `MOVE … TO TABLE`, а кросс-дисковый `MOVE … TO DISK/VOLUME` — «a follow-up to verify». Предложено: либо покрыть оба направления, либо отбивать кросс-дисковый случай `SUPPORT_IS_DISABLED`, как сделано для прочих загейченных партишен-операций.

### Что на HEAD дословно

`src/Storages/MergeTree/MergeTreeData.cpp:6816` — `case MetadataStorageType::CAS:`; `:6839-6843` — комментарий: «NOTE: `MOVE_PARTITION` also admits cross-disk `MOVE ... TO DISK/VOLUME` (this check cannot distinguish the destination); that uses the byte-copy `clonePart` path (NOT the corrupting per-file hardlink), but only same-disk `MOVE ... TO TABLE` is verified here — cross-disk is a follow-up to verify.» `:6849` — `PartitionCommand::MOVE_PARTITION` по-прежнему в `supported_commands`. То есть буквальное утверждение обзора не устарело: ни покрытие не заявлено, ни отказ не добавлен, ни комментарий не переписан.

### Что фактически разрешено/запрещено сегодня — по направлениям

**(1) В CA (`MOVE PART|PARTITION TO DISK|VOLUME <ca disk>`): работает, это сделано специально.**
`DataPartStorageOnDiskBase::clonePart` (`:744-768`) имеет ветку `if (dst_disk->isContentAddressed())`, которая гоняет весь клон через ОДНУ транзакцию приёмника (`clone_transaction = dst_disk->createTransaction()`, `copyDirectoryContentIntoTransaction`, `commit()`, откат `undo()`), с комментарием «L2 (MOVE-to-CA fix): a content-addressed disk models a part as ONE atomic unit». Бэклог-пункт `BACKLOG/replication.md:15` **[move-part-to-ca-architecturally-unimplemented]** помечен «✅ CLOSED at HEAD by L1 (`2f2a3b01aa6`, `4d73e198f6b`, `81eab8b6968`) + L2 (`4229a1477be`) — verified 2031-triage CAS-120». Динамическое подтверждение есть и оно именно кросс-дисковое: сценарии S36 (`ALTER TABLE s36_move MOVE PART … TO DISK 'ca'`) и S37 (TTL-move в CA-volume) — `utils/ca-soak/scenarios/RUN_HISTORY.md:424-425` и `:466-467`, оба `pass` 2026-07-18 (до фикса, `:415-416`, оба `fail`). После 07-18 прогонов S36/S37 в истории нет.
Живые остатки этого направления: `BACKLOG/replication.md:16` **[VERIFY-ca-ca-same-pool-move]** — CA↔CA внутри ОДНОГО пула не прогонялся, открыт вопрос о доброкачественности коллизии ref-а `<part>` у источника и приёмника; `:17` **[killed-mid-move-partition-duplicate]** — kill посреди `MOVE PARTITION` оставляет задублированную партицию, атрибутировано как PRE-EXISTING и вероятно generic, не CA-специфика.

**(2) Из CA (`MOVE … TO DISK|VOLUME <не-CA диск>`): достижимо, гарда нет, и здесь реальный дефект.**
`clonePart`'s `else`-ветка (`:769-782`) уходит в `src_disk->copyDirectoryContent(...)`, то есть в `DiskObjectStorage::copyFile` (`src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:291`), которая на `:300` сравнивает `getDataSourceDescription() == to_disk.getDataSourceDescription()` и при равенстве делает СЕРВЕРНУЮ копию (`transaction->copyFile`, `:307`), иначе — буферный `IDisk::copyFile` (`:321`). Ключевой факт: `metadata_type` в это сравнение НЕ входит — `DataSourceDescription::operator==` (`src/Disks/DiskType.cpp:35-38`) сравнивает `type, object_storage_type, description, is_encrypted, zookeeper_name`, а `metadata_type` объявлен отдельным полем (`src/Disks/DiskType.h:33`, `:49`) и в кортеж не попал. Поэтому CAS-s3 и plain-s3 на одном эндпойнте считаются одним источником данных, и берётся серверная копия, которая читает ключи через `getStorageObjects`, теряющий смещение payload (`ContentAddressedMetadataStorage.cpp:1859-1865`, комментарий `:1860-1864` — «StoredObject carries no range»), то есть копирует байты КОНВЕРТА.
Это ровно 2031-триаж **CAS-020** (`docs/superpowers/cas/2031-triage.md:726-870`, статус подтверждено, P2, бэклог {#move-out-copies-envelope-bytes}). Существенное смягчение оттуда же (`(d) Громко или тихо`): в любой реальной части есть файлы <= `INLINE_CAP` 1 MiB (`count.txt`, `columns.txt`, `checksums.txt`), для них `getStorageObjects` отдаёт ПУСТОЙ ключ (`:1828-1829`), бэкенд падает, `clonePart` ловит и вычищает приёмник (`DataPartStorageOnDiskBase.cpp:766-772`). Итог операции — громкий отказ с невнятным сообщением плюс мусорные объекты в приёмнике, а не тихая порча. Когда `DataSourceDescription` РАЗЛИЧАЮТСЯ (типичный CA-s3 → local), берётся буферный путь через `prepareRead`, и копия корректна — именно поэтому `BACKLOG/replication.md:15` пишет «unaffected: … off-CA moves (CA→local)»; триаж CAS-020 уточняет, что это верно только при неравных описаниях.

**(3) `MOVE PARTITION … TO TABLE` / `REPLACE PARTITION` между разными политиками: отбиваются заранее.**
`StoragePolicy::isCompatibleForPartitionOps` (`src/Disks/StoragePolicy.cpp:420-435`) требует, чтобы ВСЕ диски обеих политик были `isPlain()`; CAS-метахранилище `isPlain()` не переопределяет (дефолт `false`), поэтому `must_on_same_disk=true` и запрос падает `BAD_ARGUMENTS` до всякого копирования (разбор — `2031-triage.md:2656`). То есть «кросс-дисковость» реально достижима только через `TO DISK/VOLUME` и TTL-перемещения, что и описано в (1)/(2).

### Покрытие
Stateless-тест на CA-партишен-операции есть, но он про TO TABLE: `tests/queries/0_stateless/04280_cas_clone_partition_works.sql:38` (`ALTER TABLE t_cas_clone_src MOVE PARTITION 3 TO TABLE t_cas_clone_dst`). Ни одного stateless-теста с `MOVE … TO DISK|VOLUME` на CA-диск (в любую сторону) нет — единственное динамическое покрытие кросс-дискового MOVE живёт в soak-сценариях S36/S37, и только для направления В CA.

### Что осталось
1. (P2, главное) Гард на стороне ИСТОЧНИКА для серверной копии из CA — структурно это либо включить `metadata_type` в `DataSourceDescription::operator==`/`sameKind`, либо явный CA-чек перед `transaction->copyFile`. Отслежено как CAS-020; правка затрагивает общий (апстримный) файл, значит по стоячему правилу требует согласования.
2. (P2) Привести комментарий `MergeTreeData.cpp:6839-6843` в соответствие с фактом: направление В CA теперь реализовано и прогнано (L1+L2, S36/S37), направление ИЗ CA — незакрытый гард. Сейчас комментарий одинаково «не проверено» про обе стороны и потому дезинформирует.
3. (P3) Прогнать `[VERIFY-ca-ca-same-pool-move]` (S37 CA↔CA leg) и добавить stateless-тест на `MOVE PART … TO DISK '<cas>'`, чтобы кросс-дисковое направление не держалось на одном soak-прогоне месячной давности.
Отбивать `MOVE_PARTITION` через `SUPPORT_IS_DISABLED`, как предлагал обзор, уже неверно: это сломает реализованную и проверенную MOVE-to-CA функциональность.

## M12 — Публичные доки ссылаются на несуществующие артефакты и недоописывают настройки (подтверждено, P2) {#m12}

**Все три подпункта на HEAD воспроизводятся дословно и ни один не исправлен; 12c при перепроверке ОКАЗАЛСЯ ШИРЕ заявленного — неверный префикс `ca-` вместо `cas-` встречается не только в `roadmap.md`, а в шести файлах публичной документации (9 мест), так что фраза обзора «every other page is correct» неверна.**

Заявлено (обзор, п.12): (12a) `correctness.md` — официальная страница «как проверялась безопасность CAS» — ссылается на корпус TLA+ по пути `docs/superpowers/models/` и на chaos-харнесс `utils/ca-soak/`, которых в поставляемом дереве нет (ноль `.tla`-файлов); плюс семь исходников и `programs/disks/CommandCaGcRebuild.cpp` ссылаются на `docs/superpowers/cas/BACKLOG.md`/`AGENTS.md`. (12b) `configuration.md` заявляет, что перечисляет ВСЕ настройки, сгенерированные из `ContentAddressedSettings` на HEAD, но пропускает 10 из 29 — ровно GC-бюджеты, нужные оператору при массовом `DROP`. (12c) `roadmap.md` даёт неверные имена команд `clickhouse-disks` (`ca-fsck` … вместо зарегистрированных `cas-fsck` …).

### 12a — подтверждено, и различие «ветка разработки vs поставляемая» здесь ключевое

Текст на месте: `docs/en/antalya/cas/architecture/correctness.md:20-21` — «The full model index (source `.tla` files and proof-run records) lives at `docs/superpowers/models/`; this page is the reader-facing summary»; `:25` — заголовок таблицы «Model (`docs/superpowers/models/`)» с 12 строками, каждая называет конкретный `.tla`; `:42` — «the soak/chaos harness (`utils/ca-soak/`)»; `:52` — «(`utils/ca-soak` scenario S30 …)».

На ЭТОЙ ветке (`cas-gc-rebuild`) оба пути существуют и версионированы: `git ls-files docs/superpowers` → 644 файла, из них 148 `*.tla`; `git ls-files utils/ca-soak` непуст. На дереве, которое обозревали (`056488b47a0`), их нет: `git ls-tree -r --name-only 056488b47a0 -- docs/superpowers` → 0 записей, то же для `utils/ca-soak`. То есть претензия обзора верна именно для поставляемого артефакта, и суть её не «пути не существуют», а «публичная страница адресует читателя в дерево разработки, которого он не получит». Это не самоисправится: `docs/superpowers/` — рабочая зона и в релизную ветку не едет по построению.

Ссылки из кода: их стало МЕНЬШЕ (было заявлено 8, на HEAD 5, `git grep -n "docs/superpowers" -- src programs`):
- `programs/disks/CommandCaGcRebuild.cpp:20` — «see docs/superpowers/cas/04-gc-protocol.md#gc-rebuild»; этого файла НЕТ уже и на ветке разработки (консолидация 2026-08), т.е. ссылка битая дважды;
- `src/.../Gc/CasGc.cpp:1569` и `src/Storages/StorageTableProxy.h:62` — на `docs/superpowers/cas/BACKLOG.md`;
- `src/.../Tools/CasFsck.h:239` — на `docs/superpowers/cas/AGENTS.md`;
- `src/Disks/tests/gtest_cas_parallel_commit.cpp:11` — на `docs/superpowers/sdd`, каталог удалён (`e8ecc2c5bdc`).
Все пять — ещё и нарушение стоячего правила «комментарий не цитирует планы/BACKLOG/ревью»: причину надо оставить, происхождение убрать.

### 12b — подтверждено ровно в заявленном объёме, без изменений

`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp` содержит ровно 29 `DECLARE(...)`. Сверка каждого имени с `docs/en/antalya/cas/configuration.md` даёт 10 отсутствующих, и это именно бюджеты темпа GC:
`gc_round_graduation_budget`, `gc_round_handoff_prefix_wholesale_budget`, `gc_round_outcome_entry_budget`, `gc_round_prefix_wholesale_budget`, `gc_round_redelete_budget`, `gc_round_ref_cleanup_budget`, `gc_round_sweep_namespace_budget`, `gc_round_sweep_recovery_op_budget`, `manifest_sweep_delete_budget_keys`, `manifest_sweep_list_budget_keys`.
Заявление о полноте живёт прямо во frontmatter страницы: `docs/en/antalya/cas/configuration.md:2` — «Every disk-level and server-level setting content-addressed storage exposes, generated from ContentAddressedSettings and ServerSettings at HEAD». Слово «generated» тут вводит в заблуждение вдвойне: генерации нет, есть ручной список, и проверки счётчика (тест/CI-гейт «число задокументированных == число `DECLARE`») тоже нет.

### 12c — подтверждено И ШИРЕ, чем в обзоре

Зарегистрированные имена — `programs/disks/DisksApp.cpp:345-349`: `cas-fsck`, `cas-gc-dryrun`, `cas-gc-rebuild`, `cas-inspect`, `cas-drop-member` (и сами команды печатают себя так же: `CommandFsck.cpp:24` `command_name = "cas-fsck"`, `CommandCaGcRebuild.cpp:34`).
Неверный префикс `ca-` в `docs/en/antalya/cas/` — 9 мест в 6 файлах, а не только в `roadmap.md`:
- `roadmap.md:49` — «`clickhouse-disks` commands `ca-fsck`, `ca-inspect`, `ca-gc-dryrun`, and `ca-gc-rebuild`» (4 имени в одной строке);
- `architecture/correctness.md:50` — «`clickhouse-disks ca-fsck` plus `ca-gc-dryrun`»;
- `architecture/garbage-collection.md:246`, `:256` («`clickhouse-disks ca-gc-rebuild`»), `:257` («`clickhouse-disks ca-gc-dryrun`»);
- `architecture/read-path.md:78` — «(`ca-fsck`, `ca-gc-dryrun`, and similar tools)»;
- `architecture/blob-protocol.md:174`, `architecture/replication.md:89`, `architecture/manifests-and-refs.md:244` — `ca-fsck` в прозе.
Уточнение обзора «every other page is correct» на HEAD не соответствует действительности; правка — не 4 имени в одном файле, а сплошной проход по шести файлам.

### Что осталось (весь пункт открыт)
1. Переписать `correctness.md` так, чтобы публичная страница не адресовала читателя в дерево разработки: либо описать корпус моделей и soak без путей (утверждение о том, ЧТО доказано, ценно и без ссылки), либо отгрузить публичное подмножество. Заодно — снять пять ссылок `docs/superpowers/*` из `src/`/`programs/` (одна из них уже битая и на самой ветке разработки).
2. Дописать 10 GC-бюджетов в `configuration.md` и добавить проверку счётчика, иначе список разъедется снова.
3. Сплошная замена `ca-` → `cas-` в именах команд по шести файлам `docs/en/antalya/cas/` (9 мест).
P2 и не pre-release: это документация, ни одно из трёх не влияет на поведение сервера, но 12b — операционный риск в инциденте (оператор не найдёт ручки темпа GC), а 12c — команды, которые буквально не запускаются в том виде, в каком напечатаны; починка целиком механическая.

## M13 — Пробелы наблюдаемости и покрытия под собственные классы риска (подтверждено, P2) {#m13}

**Все три названные наблюдаемости/покрытия дыры на HEAD воспроизводятся: у `throwCasWriteRetryLater` по-прежнему только rate-limited WARNING и ни одного `ProfileEvent` при 63 call-site'ах, в `GcHealth`/`cas_mounts` нет сигнала «GC остановлен администратором» (есть только `is_leader`), а перечисленные тестовые пробелы подтверждаются пересчётом; поправка обзора про `gtest_cas_gc_stop_start.cpp` тоже верна — 10 gtest'ов есть, SQL-уровня нет.**

Заявлено (обзор, п.13): (а) у класса retry-later нет счётчика — 40+ call-site'ов в 6 файлах сходятся в один логгер с 30-секундным rate limit и без `ProfileEvent`, оператор не может строить тренд/алерт по write-contention; добавить агрегатный `ProfileEvent` в `throwCasWriteRetryLater`. (б) Нет сигнала, отличающего «GC остановлен администратором» от «не лидер» — `GcHealth`/`cas_mounts` отдают только `is_leader`; добавить `gc_scheduler_running`. (в) Не покрыто: мисконфигурация диска (ноль `serverError`/`clientError` в ~34 не-access CAS stateless-тестах), настоящая crash-consistency (failpoint только в момент коммита, реальный SIGKILL только МЕЖДУ запросами, ноль `KILL QUERY` посреди INSERT), межпроцессные/многоклиентские гонки (все 10 интеграционных наборов гоняют серверы последовательно, ни одного `threading`), ни одна perf/stress/fuzzer/upgrade полоса не идёт на CAS-дефолтном хранилище, cross-disk-type `MOVE`/backup, `FORGET`-на-живом. Плюс собственная поправка обзора: `SYSTEM CAS GC STOP/START` хорошо покрыт на уровне gtest.

### (а) ProfileEvent на retry-later — подтверждено, без изменений

`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:183-188` — `logCasWriteRetryLater` содержит ровно `LogSeriesLimiter log(getLogger("CasWriteRetryLater"), 1, 30); LOG_WARNING(...)` и ничего больше; `:190-194` (`throwCasWriteRetryLater`) и `:196-201` (`makeCasWriteRetryLaterExceptionPtr`) вызывают только его. `ProfileEvents` в этом файле есть и активно используются (`:66`, `:74`, `:77`, `:80` — `CASConditionalWrite*`; `:412`, `:472`, `:538`, `:566` — `CASConditionalWriteFenceLostPostWrite`), то есть отсутствие счётчика именно здесь — пропуск, а не отсутствие механизма.
Масштаб даже больше заявленного: `git grep -o "throwCasWriteRetryLater(" -- src` → 63 вхождения; по файлам — `Pool/CasRefLedger.cpp` 55, `Pool/CasPartWriteTxn.cpp` 11, `Pool/CasRefCatalog.cpp` 4, `Gc/CasGc.cpp` 2, `Pool/CasRefCkpt.cpp` 2, `Gc/CatalogLifecycleReconciler.cpp` 1 (плюс 2 в самом `CasRequestControl.cpp` и 3 в тесте).
Важное уточнение к формулировке обзора «the one printed line may not even name the active cause»: комментарий на месте (`:177-181`) сам это признаёт — «`LogSeriesLimiter` keys on the LOGGER NAME only, so under a sustained outage where `why` keeps changing slightly, only the first message in each window prints; this is the intended throttle». То есть при сохранении текущего троттлинга агрегатный счётчик — единственный способ увидеть класс целиком, и именно его нет.

### (б) `gc_scheduler_running` — подтверждено, и уже заведено в бэклог другим триажем

`Gc/CasGcScheduler.h:123-130` — `struct GcHealth { bool is_leader; bool ever_succeeded; Int64 pending_reclaim; UInt64 last_success_age_seconds; UInt64 wedged_namespace_count; }` — поля про остановку нет. `src/Storages/System/StorageSystemContentAddressedMounts.cpp:52` объявляет `is_leader` («1 if this server's GC scheduler holds this disk's leadership lease»), заполняется на `:199`; больше ничего про состояние планировщика таблица не отдаёт. `SYSTEM CAS GC STOP` — stop-in-place (`src/Interpreters/InterpreterSystemQuery.cpp:2666-2682`, `ca->gcStop()`), единственный след — одноразовый `LOG_INFO` на `:2682`. Грепа `gc_scheduler_running`/`scheduler_running` по `src` нет вовсе.
Пункт уже описан независимо: `BACKLOG/operability-and-introspection.md:491-498` (2031-триаж CAS-098, пункт 2) — «`is_leader = 0` conflates "follower", "operator stopped GC here" and "scheduler self-exited" … A `gc_running` (or `gc_state`) column, sampled from the scheduler's `stopping` latch, closes it». Там же соседний, того же семейства и НЕ упомянутый обзором дефект: `ever_succeeded` вычисляется, но не отдаётся ни колонкой, ни метрикой, из-за чего алерт `CASGCLastSuccessAgeSeconds_<disk> > threshold` не может сработать для диска, чей GC не преуспел ни разу.

### (в) Перечисленные тестовые пробелы — пересчитаны на HEAD

- **Мисконфигурация диска.** CAS stateless-тестов (семейства `04xxx_cas_/04xxx_content_addressed*` и `05xxx_cas*`) — 38 файлов. `serverError`/`clientError` встречается в 5, из которых 4 — access-тесты (`05011_cas_gc_rebuild_access.sh`, `05016_cas_drop_pool_member_access.sh`, `05019_cas_fsck_access.sh`, `05022_cas_verb_access.sh`), а пятый — одна строка `05007_cas_gc_introspection.sh:105` (`SYSTEM CAS GC RUN 'default'; -- { serverError BAD_ARGUMENTS }`, т.е. не-CA диск). Заявление обзора («ноль в не-access тестах») на HEAD верно с точностью до этой одной ассерции: путей ошибок конфигурации CA-диска не проверяет ни один тест.
- **Crash-consistency.** Failpoint-покрытие ровно в момент коммита: `tests/integration/test_cas_insert_fault_recovery/test.py:64` — `disk_object_storage_fail_commit_metadata_transaction`. Реальный SIGKILL один и он МЕЖДУ запросами: `tests/integration/test_cas_shared_pool/test.py:265` — `node1.stop_clickhouse(kill=True)` вызывается после того, как INSERT уже вернулся (`:255-261`). `KILL QUERY` не встречается ни в одном CAS-тесте (`git grep -ln "KILL QUERY" -- tests/integration/test_cas* tests/queries/0_stateless/05*` → пусто).
- **Многоклиентские гонки.** Интеграционных наборов теперь 11 (`test_cas_drop_pool_member`, `test_cas_file_cache`, `test_cas_gcs`, `test_cas_gc_s3`, `test_cas_gc_sharded`, `test_cas_insert_fault_recovery`, `test_cas_lazy_load_recovery`, `test_cas_ref_snaplog`, `test_cas_replicated_relink`, `test_cas_s3`, `test_cas_shared_pool`). `threading`/`ThreadPoolExecutor` встречается только в `test_cas_gcs/gcs_mocks/{auth,server}.py` — это mock-сервер GCS, то есть инфраструктура, а не драйвер теста: ни один набор не запускает конкурентных клиентов. Пробел подтверждается; он же зафиксирован под другим углом в `BACKLOG/testing-and-ci.md:19` **[review #14]** («concurrency invariants validated only by sequential-logic tests»).
- **Полосы CI.** CAS-параметризация есть ТОЛЬКО у функциональных тестов: `ci/defs/altinity_jobs.py:74-121` (`cas storage` / `cas s3 storage`, включая asan/tsan/msan шарды) и `:126-131`, где эти джобы добавляются к `JobConfigs.functional_tests_jobs`. Ни perf, ни stress, ни fuzzer, ни upgrade-полосы CAS-варианта не имеют. Подтверждено.
- **Cross-disk-type MOVE/backup.** BACKUP на CA покрыт одним тестом и только на локальный бэкап-диск: `tests/queries/0_stateless/05005_cas_backup_restore.sh:16` — `backup_name="Disk('backups', ...)"`; ни `S3(...)`, ни бэкапа между разнотипными дисками нет. По MOVE — см. отдельный разбор в M11: stateless-теста на `MOVE … TO DISK/VOLUME` с CA-диском нет вообще, единственное покрытие — soak-сценарии S36/S37 и только в направлении В CA.
- **`FORGET`-на-живом.** `SYSTEM CAS FORGET` встречается в `05019`, `05020`, `05022`, `05023`, но везде как teardown после дропа всех таблиц: `05023_cas_dropns_leaked_namespace.sh:185-196` — «(7) fail-closed teardown … FORGET the disk (all tables already dropped above)». Сценария «FORGET при живых таблицах/запросах» нет. Подтверждено.
- **Поправка обзора про GC STOP/START — верна.** `src/Disks/tests/gtest_cas_gc_stop_start.cpp` существует и содержит 10 `TEST`; на SQL-уровне же покрытие только привилегиями — `SYSTEM CAS GC STOP|START` в `tests/` встречается лишь в `01271_show_privileges.reference` и `05022_cas_verb_access.sh`, то есть функционального SQL/интеграционного теста действительно нет.

### Что осталось (весь пункт открыт, разными кусками)
1. (P2, дёшево) Агрегатный `ProfileEvent` в `logCasWriteRetryLater` — одна строка в единственной общей точке, покрывающая все 63 call-site'а; желательно с разбиением writer/GC, раз оба класса туда сходятся.
2. (P2, уже описано в CAS-098) Колонка `gc_running`/`gc_state` в `system.cas_mounts` из латча `stopping`, заодно закрыть `ever_succeeded`.
3. (P3, разное) Тестовые пробелы: error-path на мисконфигурацию CA-диска; `KILL QUERY` посреди INSERT; хотя бы один интеграционный набор с конкурентными клиентами; функциональный тест на `GC STOP/START`; BACKUP на S3-бэкап-диск с CA-источника; `FORGET`-на-живом.
Не pre-release: ни один пункт не меняет корректности, все — про диагностируемость и про то, какие классы риска сегодня некому поймать; но пункт 1 и 2 стоит взять первыми, потому что именно они превращают инцидент в «нет данных».

## Minor issues — детали {#minor-details}

### m1 (подтверждено, P3) {#m1}

На HEAD все четыре integrity-находки бросают `ErrorCodes::BAD_ARGUMENTS`: `programs/disks/CommandFsck.cpp:146-147` («{} reachable object(s) MISSING (INV-NO-LOSS violation)»), `:153-154`, `:164-165`, `:175-176`. Ровно тот же код используется и для CLI-ошибок в том же файле (`:45`, `:49`, `:52-53`), так что exit-code-контракт не различает «плохой аргумент» и «пул повреждён». Правка тривиальна — `CORRUPTED_DATA` на четырёх сайтах; фиксов в истории нет.

### m2 (подтверждено (смежно с дубликат CAS-096), P3) {#m2}

`programs/disks/CommandCaGcRebuild.cpp:67-70` выводит `performed/round/generation/namespaces/shards/committed_refs/live_precommits/unowned_alive_manifests/edges/clamped_shards`; ни `virgin_by_enumeration`, ни `adopted_seal_generation` в потоке нет (в файле они не встречаются вовсе). Поля живы в структуре (`Gc/CasGc.h:112` и соседнее) и выводятся SQL-поверхностью — `src/Interpreters/InterpreterSystemQuery.cpp:2426`/`:2446` и лог-строка `:2591-2594`. Триаж CAS-096 (`2031-triage.md:114`) как раз опроверг тезис «REBUILD сообщает только performed=1», подтвердив 13 колонок в SQL, — то есть асимметрия ровно между SQL и CLI, и остаток именно в CLI.

### m3 (подтверждено, P3) {#m3}

`src/Parsers/ParserSystemQuery.cpp:504-524`: ветка `Type::CAS_DROP_POOL_MEMBER` парсит srid и диск двумя `ParserStringLiteral`, минуя `parseQueryWithOnClusterAndTarget`. Соседние `CAS_FSCK/FORGET/GC STOP/GC START` (`:495-503`) и `CAS_GC_REBUILD` идут через `SystemQueryTargetType::Disk` и принимают голый идентификатор. Комментарий в коде обосновывает строковый литерал для srid (opaque path с `/`), но на диск это обоснование не распространяется — грамматическая непоследовательность живая, фикса в истории нет.

### m4 (подтверждено, P3) {#m4}

`src/Interpreters/InterpreterSystemQuery.cpp:2533-2560` (`runContentAddressedGcRun`): ветка пустого `disk_name` идёт `for (const auto & [name, disk] : getContext()->getDisksMap())` и на каждом `tryFromDisk`-успехе вызывает `ca->runGarbageCollectionRoundNow()`, добавляя строку. Ни `server_root_id`, ни указатель `ContentAddressedMetadataStorage*` не используются как ключ дедупликации, так что cache-обёртка над CA-диском даёт две записи в map и два синхронных раунда (LIST/fold/delete дважды) плюс вводящий в заблуждение двухстрочный результат. Живой двойной раунд по-прежнему не прогонялся — подтверждена только форма кода.

### m5 (дубликат CAS-033, P2) {#m5}

На HEAD место переехало в `Gc/CasGc.cpp:1301-1324` (`Gc::foldManifestEdges`): `decodePartManifest(...)` на `:1319` и два `throw Exception(ErrorCodes::CORRUPTED_DATA, ...)` на `:1320-1324` (ref mismatch / namespace mismatch) не обёрнуты ничем — исключение поднимается из fold и рвёт раунд целиком. Контраст с ref-логом сохранился (`gtest_cas_gc_fold.cpp:165` `RefMismatchFailsClosed`). Триаж 2031 уже адъюдицировал обе половины: CAS-033 признал пул-широкий destructive gate осознанным fail-closed выбором с трекингом гранулярности в BACKLOG, а CAS-040 (P1, до релиза) фиксирует именно «один битый/осиротевший манифест навсегда заклинивает GC по всему пулу». Отдельного действия по m5 не нужно.

### m6 (подтверждено, P3) {#m6}

`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:18`, `:21`, `:25` заводят три `DataTypeEnum8` (`type_enum`, `outcome_enum`, `trigger_enum`) с явными числовыми значениями, при том что в том же файле уже есть `lc_string = LowCardinality(String)` (`:27`) — то есть смешение стилей внутри одной таблицы. Риск ровно тот, что описан в обзоре: при форк-ребейзе перенумерация значений молча меняет смысл уже записанных данных. Ни правила append-only, ни перехода на `LowCardinality(String)` на HEAD нет.

### m7 (подтверждено, P3) {#m7}

Третья строка `Primitives/CasTypes.h` — `#include <.../ContentAddressed/Formats/CasFormat.h>`, а `:160` вызывает `storedSuffix(FormatId::PartManifest)` внутри `fmt::format("{:06}{}", manifest_ordinal, ...)`. Правило зафиксировано в `.../ContentAddressed/README.md:77` (слоёвка `Primitives → Formats → Backend → Pool → Gc → Tools`) и `:80` («**`Primitives/`** — the vocabulary, zero outward dependencies»). CI-проверки слоёв нет, так что нарушение держится с момента разбиения (`592b9b83568`); лечится либо переносом `storedSuffix`, либо переносом самого хелпера ключа в `Formats/`.

### m8 (подтверждено, P3) {#m8}

`src/IO/ReadBufferFromFileView.cpp:169-179`: при `file_offset_of_buffer_end > getRightBound()` делается `working_buffer.resize(max(size - extra, 0))` и `file_offset_of_buffer_end = getRightBound()`, при этом `pos` не переставляется — то есть `pos` может оказаться за новым `end()`, после чего `available()` завернётся. Все четыре вызывающих на HEAD (конструктор `:30`, `setReadUntilPosition` `:56`, `setReadUntilEnd` `:70` — с новой rebase-логикой `:45-55`/`:62-69`) по-прежнему сужают границу до/вместе с seek, так что гап остаётся сугубо латентным. Дешёвая страховка — `if (pos > working_buffer.end()) pos = working_buffer.end();` внутри самой функции.

### m9 (дубликат CAS-038, P3) {#m9}

`Formats/CasGcOutcomesFormat.cpp:100-113` заводит `have_ha/have_h/have_tt`, а `tv` читается без флага (`:108`) и проверка на `:113` требует только `ha/h/tt` — при этом `encodeOutcomeLog` всегда пишет `tt`+`tv` парой (`writeTokenFields(out, first, e.token)`), так что усечённая запись декодируется с пустым токеном. Триаж 2031 (CAS-038, `2031-triage.md:56`, тело `:1858+`) прошёл по семи подпретензиям того же класса, пять признал не соответствующими HEAD и оставил как реальные остатки необязательный `oc` в этом же декодере (`:113-114`) и отсутствие line-cap на записи `gc/state` — оба классифицированы как косметика, P3. Правильный ход — один required-field sweep, объединив `tv` с уже заведёнными остатками CAS-038, а не отдельный пункт.

### m10 (подтверждено, P3) {#m10}

Поведение на HEAD: `programs/disks/DisksApp.cpp:238` кладёт код ошибки в `last_command_exit_code`, а `:622-623` возвращает его из `main` только для неинтерактивного режима (`query.has_value()`). Коммит — `f85cb4330c8` «clickhouse-disks: non-interactive runs exit nonzero on a failed command». Пункт отслеживается в `BACKLOG.md:190` {#disks-exit-code-upstream} как carve-out-обязательство (вынести правку отдельным upstream-PR), но именно строки changelog/release-note там нет; ближайший прецедент оформления — `BACKLOG/docs-and-cleanup.md:76` {#CHANGELOG-unknown-config-key-rejection}.

### m11 (подтверждено, P3) {#m11}

Изменение живо: `src/Interpreters/InterpreterSystemQuery.cpp:278` несёт комментарий про таблицу в БД с `lazy_load_tables = 1`, остающуюся обёрнутой в `StorageTableProxy`; коммит — `2ba28ac4b6f` «SYSTEM SYNC REPLICA (and sibling per-table SYSTEM verbs) materialize lazy_load_tables proxies instead of failing "is not replicated"» (+ stateless-тест 05017). Тема широко отслежена в `BACKLOG/operability-and-introspection.md` — `:105` фиксирует и фикс, и открытый эмпирический хвост, `:106`/`:119` — оставшиеся proxy-forwarding-пробелы (`DROP REPLICA`, `RESTART REPLICAS`, `STOP/START <action>`), а `:80` держит USER DECISION по самой фиче. Отсутствует ровно то, о чём буллет: changelog-запись про смену поведения.

Примечание: в исходном списке одиннадцать буллетов, а не двенадцать — строка `m12` из таблицы удалена.
