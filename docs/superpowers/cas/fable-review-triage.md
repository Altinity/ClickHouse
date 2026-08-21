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
| M5 | `ContentAddressedTransaction` стейджит записи манифеста за O(F²) | ⏳ | — | — | — |
| M6 | Blob-upload pool: сырая ссылка убегает из-под лока; `clickhouse-local` рушит порядок teardown | ⏳ | — | — | — |
| M7 | Инлайновые CAS-диски не сносятся на `DROP TABLE` (утечка lease + потоков) | ⏳ | — | — | — |
| M8 | Сетевой деструктор `Pool` может выполняться под `pointer_mutex` | ⏳ | — | — | — |
| M9 | GCS `gcp_oauth` переписывает ВСЕ запросы/ответы, не только условные записи | ⏳ | — | — | — |
| M10 | Disk-transaction contract трогает 16 общих файлов MergeTree — нужен динамический non-CAS прогон | ⏳ | — | — | — |
| M11 | Кросс-дисковый `MOVE PARTITION` (CAS ↔ non-CAS) достижим, но помечен непроверенным | ⏳ | — | — | — |
| M12 | Публичные доки ссылаются на несуществующие артефакты и недоописывают настройки | ⏳ | — | — | — |
| M13 | Пробелы наблюдаемости и покрытия под собственные классы риска | ⏳ | — | — | — |

## Minor issues {#minor}

| ID | Статус | Приоритет | До релиза? | Где отслеживается | Суть |
|----|--------|-----------|------------|-------------------|------|
| m1 | ⏳ | — | — | — | — |
| m2 | ⏳ | — | — | — | — |
| m3 | ⏳ | — | — | — | — |
| m4 | ⏳ | — | — | — | — |
| m5 | ⏳ | — | — | — | — |
| m6 | ⏳ | — | — | — | — |
| m7 | ⏳ | — | — | — | — |
| m8 | ⏳ | — | — | — | — |
| m9 | ⏳ | — | — | — | — |
| m10 | ⏳ | — | — | — | — |
| m11 | ⏳ | — | — | — | — |
| m12 | ⏳ | — | — | — | — |

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
