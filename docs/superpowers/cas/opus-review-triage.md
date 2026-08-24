# Триаж umbrella review (Opus) от 2026-08-05 {#opus-review-triage}

Источник: `docs/superpowers/cas/random/opus-review-20250805.md` — второй обзор того же диапазона
(`5e8eaeb4d7d` → `056488b47a0`), 15 полос ревью, вердикт «request changes». Отличается от
`fable-review-triage.md` (обзор того же дня другой полосой) прежде всего разделом blast radius —
что регрессирует у тех, кто CAS никогда не включит.

Каждый пункт перепроверен против HEAD ветки `cas-gc-rebuild` (2026-08-22). Где пункт покрывает ту же
землю, что уже разобранный пункт `fable-review-triage.md` или `2031-triage.md`, ставится
**дубликат** со ссылкой — заново не выводим.

**Исторический снимок (2026-08-23).** Этот файл заморожен как аудит указанного выше диапазона и
датированного HEAD. Старые helper-методы, настройки и conditional blob publication внутри него —
исторические свидетельства, не current implementation guidance. Текущий контракт задают
[спецификация](/superpowers/specs/cas-unconditional-blob-publication-design),
[план реализации](/superpowers/plans/cas-unconditional-blob-publication),
[live backlog](/superpowers/cas/backlog) и [операторская документация](/antalya/cas).

Статусы: **исправлено** · **подтверждено** · **частично** · **не подтвердилось** · **by-design** ·
**дубликат**. Приоритет: P1 (до релиза) · P2 · P3 · — .

## Blast radius вне CAS — Tier 1-3 {#blast-radius}

Tier 1 = подтверждённые регрессии (T1-T2), Tier 2 = глобальные изменения поведения (T3-T7),
Tier 3 = конфигурация и упаковка (T8-T12).

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| T1 | исправлено | P3 | нет | `BACKLOG/formats-and-storage.md` (GCS Task 9, живой прогон) + `BACKLOG/docs-and-cleanup.md:2… | Кросс-ссылка точна: клиент-широкий GCS-диалект по `http_client = gcp_oauth` снят, следствие про Iceberg `version-hint.text` закрыто отдельно. |
| T2 | подтверждено | P2 | да | `BACKLOG/operability-and-introspection.md:707` {#disks-exit-code-truncation} + {#disks-exit-… | Кросс-ссылка точна: контракт кода выхода `clickhouse-disks --query` изменён тул-широко, 8-битное усечение живо, документация не тронута. |
| T3 | подтверждено | P3 | нет | `BACKLOG/docs-and-cleanup.md:21` {#refactor-group-g} (`S3Exception::isPreconditionFailed` в … | Кросс-ссылка точна: третий дизъюнкт (поиск подстроки в сообщении сервера) жив и по-прежнему консультируется на глобальном пути ретрая всего S3-трафика. |
| T4 | частично | P2 | да (только как доказательство, не как правка кода) | `fable-review-triage.md` {#m10}; отдельного BACKLOG-пункта под «зафиксировать не-CAS прогон»… | Кросс-ссылка точна: статический гейтинг на HEAD держится, но динамического доказательства «на не-CAS дельты против базы нет» так и не зафиксировано. |
| T5 | подтверждено | P3 | нет | `BACKLOG/docs-and-cleanup.md:21` {#refactor-group-g} («`ReadBufferFromS3` cancel-stop (B117)… | Обрыв внешнего ретрай-цикла `ReadBufferFromS3` по отмене запроса на HEAD жив, безусловен для всех S3-читателей, нигде не задокументирован. |
| T6 | частично | P3 | нет | `BACKLOG/docs-and-cleanup.md:21` {#refactor-group-g} («`MergeTreeDeduplicationLog` null-writ… | Изменение на HEAD живо и безусловно, но формулировка обзора «converts a silent pass into a throw» неверна — в апстриме на этом месте был `chassert` плюс безусловное разыменование, т.е. в release-сборке падение. |
| T7 | подтверждено | P3 | нет | `BACKLOG/docs-and-cleanup.md:21` {#refactor-group-g} («`ThreadStatus parent_thread_group` (B… | Удержание родительской `ThreadGroup` дочерней группой на HEAD живо и безусловно для всех запросов; следствие «поздний лог peak memory из фонового потока» не смягчено и не задокументировано. |
| T8 | подтверждено | P3 | нет | не отслеживается (в `opus-review-triage.md` таблице m28 стоит ⏳; отдельного BACKLOG-пункта нет) | Кросс-ссылка точна: `initializeBlobUploadPool` вызывается безусловно и бросает `BAD_ARGUMENTS` на нуле, так что CAS-only настройка `cas_blob_upload_pool_size = 0` не даёт стартовать и не-CAS серверу — но запрет описан в самой настройке. |
| T9 | частично | P3 | нет | смежно `BACKLOG/operability-and-introspection.md:533` {#cas-event-sink-installed-when-log-di… | Секции `<cas_log>`/`<cas_gc_log>` действительно поставляются включёнными, но «две пустые таблицы материализуются на каждом сервере» — неточно: таблица создаётся только при первой записи или при `SYSTEM FLUSH LOGS`. |
| T10 | частично | P3 | нет | `BACKLOG/docs-and-cleanup.md:21` {#refactor-group-g} («`LocalObjectStorage` TOCTOU (B38)»); … | Главная часть — snapshot-семантика листинга — УШЛА В АПСТРИМ и уже присутствует в базе ветки, так что дельты по ней нет; локальным остался один шестистрочный хунк. |
| T11 | подтверждено | P3 | нет | не отслеживается (NV-14 обзора; отдельного пункта в BACKLOG нет) | Разворачивание прокси в одно-табличных `SYSTEM`-командах на HEAD живо и безусловно для пользователей `lazy_load_tables`; заявление «каждая одно-табличная точка покрыта» на HEAD уже неточно — нашёл непокрытый `dynamic_cast`. |
| T12 | исправлено | — | нет | — (закрыто) | Новая style-проверка удалена из общего скрипта коммитом `eee9a2b8a11`; файл на HEAD побайтово совпадает с базой. |

## Блокеры B1-B9 {#blockers}

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| B1 | исправлено | P3 | нет | `docs/superpowers/cas/BACKLOG/formats-and-storage.md:28` **[GCS production-grade follow-ups]… | Диалект больше не включается на весь клиент по `http_client = gcp_oauth`, и заявленная регрессия Iceberg `version-hint.text` закрыта конкретно — её запрос идёт как `Default` и проходит по upstream-пути байт-в-байт. |
| B2 | частично | P2 | нет | `docs/superpowers/cas/BACKLOG.md:422` **[gcs-conditional-overwrite-rethink]** {#gcs-conditio… | Головная половина подтверждена дословно — cap жив, `NOT_IMPLEMENTED` классифицируется как детерминированный локальный отказ и пробивает ретрай-петлю, ни Compose, ни fail-closed отказа монтирования нет; вторая половина («cap = `min_upload_part_size`») на HEAD НЕ подтверждается — эта связка удалена, хотя потолок памяти остался по другому механизму. |
| B3 | подтверждено | P1 | да | — (нигде не отслеживается: грепы по `docs/superpowers/cas/BACKLOG.md`, `BACKLOG/*.md`, `fina… | Форма подтверждена дословно на HEAD и достижима: сильный `const ContextPtr context` (`:595`) копируется в оба синка, `Context::getContentAddressedLog` безусловно разыменовывает `shared`, а `resetSharedContext()` обнуляет его до выхода последнего CAS-события — прощального `MountRelease` из `~Pool` на detached-нити (B4). |
| B4 | подтверждено | P1 | да | — по существу не отслеживается. Смежные записи, ни одна из которых этого не покрывает: `docs… | Оба заявленных detached-дispatch'а живы, по-прежнему не трекаются и держат сильную ссылку на `Pool`; `shutdown()` их не дренирует, поэтому `~Pool` с прощальной долговечной записью и эмитом события штатно может исполниться после того, как object storage уже погашен, а `Context` обнулён (B3). rev.8 закрыл соседнюю половину (само-выход GC-нитей), но не эту. |
| B5 | подтверждено | P2 | нет | — сам пункт не отслеживается. Класс частично разобран соседними триажами: `docs/superpowers/… | Механика подтверждена дословно и потолок пересчитан на текущих константах — ровно 146 000 мс; стоп-флаг внутри попытки не опрашивается, сон — голый `std::this_thread::sleep_for`, а самомаскирующий комментарий про «bounded to one step + one backend timeout» после rev.8 стоит уже в ДВУХ местах и по-прежнему неверен. |
| B6 | дубликат CAS-047 | P3 | нет | [{#writepath-candidates-post-stage1}](docs/superpowers/cas/BACKLOG/performance.md#writepath-… | Форма кода верна и на HEAD (один процессный пул, 16 потоков, `queue_size == max_threads`, блокирующий enqueue), но это штатный backpressure, уже адъюдицированный как by-design в CAS-047; остаток — отсутствие проверки отмены в ожидании (P3). |
| B7 | дубликат CAS-036 | P3 | нет | [{#control-object-read-precap-materialization}](docs/superpowers/cas/BACKLOG/formats-and-sto… | Механика на HEAD жива — тело control-объекта читается целиком до срабатывания `object_cap`, хотя авторитетный размер уже известен из HEAD; но по урегулированной модели доверия триггер — держатель bucket-креденшла, отказ громкий, тихой порчи нет, поэтому P3 (адъюдицировано в CAS-036). |
| B8 | подтверждено | P2 | нет | [{#rebuild-cannot-recover-undecodable-gc-state}](BACKLOG/gc.md#rebuild-cannot-recover-undecodable-gc-state) | Подтверждено на HEAD: `rebuildBaseline` корректно классифицирует недекодируемый `gc/state` как свою же катастрофу, проходит гейт, а затем безусловно падает `CORRUPTED_DATA` на повторном decode внутри `acquireOrRenewLease`, так что DR-команда неработоспособна ровно в том сценарии, для которого существует. |
| B9 | дубликат fable-B4 | P3 | нет | [{#local-backend}](docs/superpowers/cas/BACKLOG/formats-and-storage.md#local-backend), пункт… | Тест по-прежнему отключён тегом, concurrent-покрытия под CA нет, атомарной публикации pointer-объекта нет — но «B66a не отслеживается нигде» опровергнуто (артефакт ветки поставки без `docs/superpowers/`), сам механизм из комментария теста устарел после B181, а класс — только local/emulated бэкенд. |

## Major M1-M12 {#majors}

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| M1 | подтверждено | P3 | нет | — (не отслеживается: грепы `isPreconditionFailedError`/`ShouldRetry`/`412` по `docs/superpow… | Третий дизъюнкт — поиск подстроки `PreconditionFailed` в сообщении сервера — на HEAD жив и по-прежнему консультируется в глобальном `Client::RetryStrategy::ShouldRetry` для всего S3-трафика; комментарий переписан лишь частично. |
| M2 | дубликат CAS-066 | P2 | нет | [{#local-backend}](docs/superpowers/cas/BACKLOG/formats-and-storage.md#local-backend) — пунк… | Дубликат CAS-066 (адъюдицировано by-design): выбор режима по типу хранилища и INFO-уровень подтверждены и на HEAD, единственный живой остаток — отсутствие оговорки «только один сервер» в `quick-start.md`/`configuration.md`. |
| M3 | подтверждено | P2 | нет | — (именно эти семь счётчиков и инверсия уровня лога не отслеживаются: `grep -rn` по всем сем… | Обе половины подтверждены дословно: ни один из семи терминальных счётчиков не упоминается в `docs/`, а `CASIdentityLost`/`CASDataRootVanished` — состояния, которые код сам называет TERMINAL — по-прежнему логируются на `LOG_WARNING`, тогда как алерт по ERROR настроен на `CASMountExclusivityViolation`. |
| M4 | дубликат CAS-098 | P2 | нет | [{#gc-health-zero-is-ambiguous}](docs/superpowers/cas/BACKLOG/operability-and-introspection.… | Дубликат CAS-098 пункт 1; ключевая проверяемая часть — «фикс уже есть, но не используется» — ПОДТВЕРЖДЕНА дословно: `ever_succeeded` вычисляется в `gcHealth` и не потребляется ни одним продакшн-читателем (только двумя gtest'ами), а SQL-колонка уже `Nullable(UInt64)` и уже вставляет NULL на peer-строках. |
| M5 | подтверждено | P2 | нет | частично — класс «настойчивый отказ confirm ⇒ шторм `NETWORK_ERROR`» отслеживается через яко… | Подтверждено дословно: строки таксономии 3 и 5b бросают `NETWORK_ERROR` мимо тормоза, все четыре top-level вызова `fetchSelectedPart` идут с `allow_ca_relink = true` по умолчанию, счётчика попыток или настройки-выключателя нет, а на двух репликах shuffle — no-op, так что постоянный отказ confirm циклится без деградации в байтовый fetch. |
| M6 | подтверждено | P2 | нет | половина «контракт изменён, релиз-нота нужна» — [{#disks-exit-code-upstream}](docs/superpowe… | Обе половины живы на HEAD: сырой код ошибки ClickHouse по-прежнему возвращается из `DisksApp::main` как POSIX-статус (усечение до 8 бит, коды кратные 256 дают `exit 0`), а `clickhouse-disks.md` не описывает ни контракт кода выхода, ни пять новых CAS-подкоманд. |
| M7 | дубликат CAS-055 + CAS-118 | P2 | нет | `docs/superpowers/cas/BACKLOG/performance.md:287` {#hardlink-per-file-forcefresh-head} (P2);… | Формы кода на HEAD верны, но головное «под дефолтами КАЖДЫЙ доступ платит HEAD» на read-path не подтверждается — тёплый `CachedForLoad`-хит обслуживается без единого обращения к бэкенду; реальный остаток (ForceFresh-HEAD на файл в `createHardLink`) уже отслежен как CAS-055. |
| M8 | подтверждено (частично дубликат fable n1 + fable M12/12a) | P2 | нет | не отслеживается как отдельный пункт; частично покрыто адъюдикациями `docs/superpowers/cas/f… | Все три канала утечки внутреннего происхождения на HEAD воспроизводятся, причём opus впервые даёт масштаб: ~239 строк с тегами `B<число>` в 74 файлах, включая ~35 файлов ОБЩЕГО апстрим-кода вне CAS-каталога. |
| M9 | подтверждено | P3 | нет | отдельного пункта нет; смежное — `docs/superpowers/cas/BACKLOG/performance.md` {#standalone-… | Подтверждено: 29 дисковых настроек CAS не содержат ни одной ручки verbosity/sampling, сток пишет каждое событие без фильтрации, и ожидаемый row-rate не описан ни в `cas_log.md`, ни в `monitoring.md`. |
| M10 | подтверждено | P3 | нет | не отслеживается (grep по `InMemoryBackend` в `BACKLOG.md`, `BACKLOG/*.md`, `final-checks-to… | «Ноль продакшн-вызывающих» проверено точно и подтверждается: ни одной конструкции и ни одной записи в реестре вне `src/Disks/tests`, при этом 598 строк fault-injection безусловно попадают в `dbms` через каталожный glob. |
| M11 | подтверждено | P2 | нет | не отслеживается (grep по `experimental`/`gate` в `BACKLOG.md`, `BACKLOG/*.md`, `final-check… | Подтверждено: ни настройки `allow_experimental_*`, ни какого-либо эквивалента нет; практический гейт сегодня — только строка в конфиге диска `<metadata_type>cas</metadata_type>` плюс проза в `docs/en/antalya/cas/index.md`, причём при регистрации диска не печатается ни одного предупреждения. |
| M12 | подтверждено | P2 | нет | не отслеживается; смежное (другой механизм того же гейта) — `docs/superpowers/cas/BACKLOG/gc… | Подтверждено: единственный дренаж — шов `mutateRefsAfterWriterCleanup` перед очередной durable ref-мутацией того же namespace; ни GC-раунд, ни монтирование, ни FSCK, ни фоновый publisher, ни teardown его не дренируют — teardown только НАБЛЮДАЕТ долг. |

## Minor m1-m31 {#minors}

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| m1 | подтверждено | P3 | нет | нет (смежно: `BACKLOG/docs-and-cleanup.md:21` {#refactor-group-g} — вынос `ReadBufferFromFil… | Форма подтверждена: на пути броска bound-check в `seek` `impl` уже переставлен, а учёт вью (`file_offset_of_buffer_end`, рабочий буфер) остаётся старым — рассогласование того же класса, что чинил B115, но живой достижимости «поймал и продолжил» не найдено. |
| m2 | дубликат CAS-019 | P2 | нет | [{#part-folder-single-flight-manifest-keying}](BACKLOG/ref-protocol.md) | Тот же single-flight по `(ns, ref)` без post-wait проверки manifest id; уже разобрано как CAS-019 («частично»): каждый выданный view внутренне консистентен, эффект — сдвиг на один репойнт на stale-терпимом `CachedForLoad`, а не смешение манифестов. |
| m3 | частично | P3 | нет | нет (соседняя гонка тех же членов — `BACKLOG/mounts-and-lifecycle.md:107` {#gc-scheduler-sto… | Форма на месте — `start()` идемпотентен по `thread`, и бросок при создании `hb_thread` оставляет раунд-поток жить без heartbeat; но на startup-пути это безвредно (RAII), достижимо только через `SYSTEM CONTENT ADDRESSED GC START`. |
| m4 | дубликат CAS-050 | P2 | нет | [{#gc-scheduler-stop-join-race}](BACKLOG/mounts-and-lifecycle.md:107) | Гонка данных на `thread`/`hb_thread` между `stop()` (join вне `mutex`) и `requestRoundSoon()` (чтение `joinable()` под `mutex`) уже адъюдицирована как CAS-050 («частично», P2, отслеживается). |
| m5 | дубликат fable {#m8} | P3 | нет | нет (в `fable-review-triage.md` M8 отмечено: покрытия в BACKLOG/final-checks нет) | На HEAD форма прежняя — `part_access.reset()`/`cas_store.reset()` в `shutdown()` выполняются под `pointer_mutex`, и при выключенном GC там же срабатывает сетевой `~Pool`; уже подробно адъюдицировано как fable M8 («подтверждено», P3). |
| m6 | частично | P3 | нет | нет | Форма верна — если `snapshotOf` бросит, COW-копия `candidate_state` разрушится при раскрутке вне `state_mutex`, вопреки инварианту, на который явно опирается `RefCowMap::materialize`; но единственный «доменный» бросок отсечён проверкой на входе, так что живой достижимости почти нет. |
| m7 | исправлено | — | — | — | Класса `SingleWriterSlot` на HEAD больше нет — он ликвидирован рефакторингом владения renewal, вместе с ним ушли и `std::string_view`-члены. |
| m8 | дубликат fable {#m6} | P2 | нет | нет | На HEAD без изменений — `blobUploadPool()` возвращает `ThreadPool &` после выхода из-под `pool_mutex`, а `shutdownBlobUploadPool()` разрушает `pool_instance`; уже адъюдицировано как fable M6 (там же вторая половина про порядок teardown в `clickhouse-local`). |
| m9 | подтверждено | P3 | нет | нет | `S3ObjectStorage::supportsRetryProfile` действительно игнорирует параметр и возвращает `true` для любого значения; сегодня безвредно (в enum ровно два значения, оба S3 поддерживает), но контракт fail-closed держится не на коде, а на размере перечисления. |
| m10 | дубликат fable {#m7} | P3 | нет | нет (в fable отмечено как неотслеживаемое) | Обратный кросс-слойный include на месте: `Primitives/CasTypes.h:3` включает `Formats/CasFormat.h` ради `storedSuffix`, нарушая объявленный в `README.md` порядок `Primitives → Formats`. |
| m11 | дубликат CAS-091 | P3 | нет | [{#checknamespace-admits-dot-segments}](BACKLOG.md) | Расхождение `Layout::checkNamespace` с `validateServerRootId` по сегментам `.`/`..` уже разобрано как CAS-091 («частично», P3): механизм реален, продовой достижимости нет — все живые источники предварительно санитизированы. |
| m12 | подтверждено | P3 | нет | нет | Пропуск guard'а подтверждён: у `CAS_DROP_POOL_MEMBER` нет проверки пустого имени диска, которая есть у всех пяти соседних verb'ов; последствие — только худшая диагностика (`getDisk("")` всё равно бросит `UNKNOWN_DISK`), фан-аута нет. |
| m13 | подтверждено | P3 | нет | нет | Реинтерпретация подтверждена: для CAS-клиента с одной попыткой `expect_continue_min_bytes == 0` трактуется как «не задано» → безусловные 1 MiB, тогда как задокументированная семантика нуля в самом HTTP-слое — «выключено полностью»; выключить `Expect: 100-continue` для CAS-записей невозможно. |
| m14 | подтверждено | P3 | нет | нет | Инвариант между двумя независимо выставляемыми полями `ClonePartParams` держится только на `chassert`; в release-сборке одновременная установка `keep_metadata_version` и `metadata_version_to_write` молча запишет новую версию. Все 7 call-site'ов действительно непересекающиеся. |
| m15 | подтверждено | P3 | нет | нет | Асимметрия синтаксиса реальна — `DROP POOL MEMBER` требует строковый литерал и для srid, и для диска, тогда как шесть соседних CAS-verb'ов принимают голый идентификатор; в коде есть обоснование, но оно покрывает только srid-половину. |
| m16 | дубликат CAS-035 | P2 | нет | [{#fold-edge-run-memory}](BACKLOG/gc.md) | Полный постраничный LIST `cas/ns/stream/` в `defer_decision` подтверждён, но он не «до вердикта зря» — он и ЕСТЬ вход вердикта; стоимость класса уже полностью отслежена как CAS-035, остаточная часть (отставание budgeted cleanup удорожает следующий раунд) записана там же. |
| m17 | by-design | — | нет | — (смежное: `docs/superpowers/cas/BACKLOG/replication.md:82`; доказательная база — `docs/sup… | Обязательный `HEAD` перед `PUT` тела блоба на HEAD стал БЕЗУСЛОВНЫМ (не «≥1 MiB»), и это защищённый шаг протокола под действующим вето пользователя — менять его нельзя. |
| m18 | подтверждено | P3 | нет | не отслеживается | `Xxh3Streamer` действительно держит состояние XXH3 в куче (`XXH3_createState`) на каждый блоб вместо встраивания по значению; путь берётся только при `blob_hash = 'xxh3-128'`. |
| m19 | подтверждено | P3 | нет | не отслеживается отдельным пунктом (смежно упомянуто в `docs/superpowers/cas/BACKLOG/replica… | CA-ветка `clonePart` по-прежнему копирует файлы части последовательно через одну транзакцию, и это самопризнанная в коде отложенная оптимизация. |
| m20 | частично | P3 | нет | смежное — `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:467` {#gc-health-ze… | Асинхронной (скрейпимой как gauge) метрики здоровья продления mount-lease действительно нет — но ProfileEvent'ов по продлению семь; вторая половина находки (гейджи `CASBlobUploadPool*` не упомянуты ни в одной доке) подтверждается дословно. |
| m21 | дубликат fable n7 | P3 | нет | — (уже разобрано в `docs/superpowers/cas/fable-review-triage.md:66,594` и `docs/superpowers/… | Комментарий «Optional (off by default)» в `ContentAddressedLog.h` устарел — обе CAS-таблицы поставляются включёнными; уже адъюдицировано как fable n7. |
| m22 | дубликат fable {#m12} 12b | P2 | нет | — (адъюдицировано в `docs/superpowers/cas/fable-review-triage.md:443-483`, подпункт 12b на `… | `configuration.md` по-прежнему не содержит ни одной из десяти перечисленных GC-бюджетных настроек — подтверждено, но это уже разобранный подпункт 12b. |
| m23 | частично | P3 | нет | не отслеживается | `mounts-and-leases.md` действительно единственная из 22 CAS-страниц без H1, но «renders with no title» не доказано — frontmatter `title` у страницы есть. |
| m24 | подтверждено | P3 | нет | не отслеживается | `design-history.md` — инженерный журнал «дороги, которые не выбрали» в публичной доке; аналога у других фич в `docs/en/` нет, но страница интегрирована штатно, так что это стилистический вопрос, а не дефект. |
| m25 | дубликат fable m6 (расширенный третьей таблицей) | P3 | нет | — (fable m6, `docs/superpowers/cas/fable-review-triage.md:49`) | Три CAS-таблицы действительно типизируют закрытые словари тремя разными способами — `Enum8`, `LowCardinality(String)`, голый `String`; fable m6 покрывал первые две, третья добавляется здесь. |
| m26 | подтверждено | P2 | да | смежное и более узкое — `docs/superpowers/cas/BACKLOG/docs-and-cleanup.md:76` [CHANGELOG-unk… | На ветке нет ни одной changelog-строки про content-addressed storage — при том что книга называет её headline-фичей; это релиз-гигиена, которую нужно закрыть до релиза. |
| m27 | подтверждено | P3 | нет | не отслеживается | Заголовок тестов действительно огромен и тянется почти во все CAS-тестовые TU, но затрагивает только время сборки тестов и соответствует общей конвенции кодовой базы. |
| m28 | дубликат T8 | P3 | нет | не отслеживается (T8 прямо это фиксирует) | `cas_blob_upload_pool_size = 0` действительно валит старт любого сервера, но это документированный fail-loud; уже адъюдицировано как T8. |
| m29 | частично | P3 | нет | не отслеживается | Проверки «`enum_count` укладывается в окно» по-прежнему нет, но и код, и число устарели: специализация теперь пришла из `antalya-26.6` (чужой коммит), а окно расширено до 512, не 255. |
| m30 | дубликат fable m1 | P3 | нет | — (fable m1, `docs/superpowers/cas/fable-review-triage.md:44,522`; смежная секция `BACKLOG/o… | Доменные находки CAS-команд `clickhouse-disks` по-прежнему летят кодом `BAD_ARGUMENTS`, из-за чего перед диагностикой печатается справка по опциям; уже адъюдицировано как fable m1. |
| m31 | подтверждено | P2 | да | не отслеживается | CI действительно закреплён на бета-образе `rustfs/rustfs:1.0.0-beta.12`, и пин шире заявленного — он же в стейтлес-лейне и во всех compose-файлах soak. |

## Needs verification NV-1…NV-14 {#needs-verification}

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| NV-1 | частично | P2 | да (дешёвая страховка `repoint_old == expected`, а не блокер) | не отслеживается (смежное — `docs/superpowers/cas/BACKLOG/performance.md:328-357` про стоимо… | Есть ли у `repointRef` предусловие «старый манифест такой, каким я его прочитал»? — Нет, предусловия действительно нет (подтверждено по коду), но достижимость двух конкурентных standalone-записей в один и тот же закоммиченный ref на одном сервере доказать не удалось: все найденные MergeTree-сайты сериализованы. |
| NV-2 | не подтвердилось | P3 | нет | не отслеживается (нужно только косметическое: лишний `LOG_ERROR` на корректном пути — см. ниже) | Может ли `abort()` хендла отменить promote, который на самом деле приземлился (Unresolved)? — Нет: удаление типизировано как `RemovePrecommit` и физически не может тронуть committed-строку, плюс есть явная проверка отсутствия. |
| NV-3 | частично | P2 | да (только в части «завести регрессионный тест в репозитории») | перечисление — `docs/superpowers/cas/fable-review-triage.md:76,651-659` (V1); тестовая часть… | Просили (а) исчерпывающее перечисление durable-записей, достижимых под `Live`, и (б) SIGSTOP-тест фенсинга — перечисление УЖЕ сделано (ровно один незафенсенный сайт), а SIGSTOP-класс покрыт только soak-харнессом, не CI-тестом. |
| NV-4 | не подтвердилось | P2 | да (но как правка устаревшего утверждения в исходнике, не как исправление механизма) | якорь `{#list-as-journal-dataloss-2026-07-25}` в живом BACKLOG отсутствует (grep по `BACKLOG… | Жив ли самозаявленный TLA+-пробел `_sab_holeylist` (confirmed relink может повиснуть на висячем blob'е)? — На HEAD дефект закрыт: интейк GC стал арифметическим, LIST понижен до подсказки; живы только устаревший комментарий в исходнике и мёртвая ссылка на BACKLOG. |
| NV-5 | подтверждено | P2 | да (правка на две строки) | не отслеживается | Правда ли, что `noexcept`-`dropRefIfMatches` вызывает `eraseView` вне `try`, и правда ли `eraseView` может бросить? — Правда и то и другое: `eraseView` аллоцирует БЕЗУСЛОВНО (строит `String`-ключ), поэтому `bad_alloc` на откате даёт `std::terminate`. |
| NV-6 | частично (по существу дубликат уже отслеживаемых CAS-118 и CAS-055) | P3 (чтение) / P2 (hardlink-путь) | нет | `docs/superpowers/cas/BACKLOG/performance.md:482-501` `{#read-path-repeated-view-lookup-per-… | `getView`/`readManifestShared` — раз на часть за запрос или раз на открытие каждого файла? — Раз на КАЖДУЮ пофайловую операцию (то есть больший множитель), но дорогая половина (`readManifestShared` = HEAD+GET) на пути ЧТЕНИЯ амортизируется тёплым view-кэшем; не амортизируется она на пути `createHardLink`. |
| NV-7 | не подтвердилось | P3 | — | не отслеживается (и не нужно) | Может ли путь, оканчивающийся на `deduplication_logs`, быть неверно классифицирован, раз цикл `i + 1 < p.size()` его не ловит? — Нет: fallback-ветка выдаёт БАЙТ-В-БАЙТ тот же `TableFilePath`, что выдала бы зарезервированная ветка, и функция действительно вызывается с голым путём-каталогом — и работает. |
| NV-8 | частично | P3 | нет | не отслеживается | Появляется ли в серверном логе строка, когда ref-lane заклинило или исчерпан бюджет conditional-write внутри фонового мержа? — Да, появляется: лог живёт не на месте вызова, а в фабрике исключения (`logCasWriteRetryLater`); частичность — в глобальном троттле 1 сообщение / 30 с на весь класс. |
| NV-9 | подтверждено (дубликат B4 в части последствия) | P1 | да | — по существу не отслеживается; отслеживается как B4, `docs/superpowers/cas/opus-review-tria… | Ждёт ли `drainRefLanesForShutdown` обнуления `pending_snapshot_publishes` по каждому `RefTableRuntime`? — Нет, не ждёт; такое ожидание есть в двух ДРУГИХ местах, так что фикс — перенос уже написанного кода. |
| NV-10 | не подтвердилось (подтверждаю RESOLVED, как и записало само ревью) | — | нет | — | Может ли соединение с неотправленным заявленным телом (после отказа на `Expect: 100-continue`) вернуться в пул? — Не может: гейт возврата в пул стоит на HEAD и опирается на `isCompleted()`. |
| NV-11 | не разрешено (по существу — вне кода) | P3 | нет | — | Переживёт ли политика «recreate only, no migration» первый отгруженный релиз? — Это продуктовое/саппортное решение; из кода не выводится, и код тут не изменился — только счётчик поколений вырос. |
| NV-12 | не подтвердилось (подтверждаю RESOLVED, как и записало само ревью) | — | нет | — | Может ли повторный fetch зациклиться на том же источнике? — Нет: выбор реплики-источника перемешивается равномерно на всех путях. |
| NV-13 | не подтвердилось | P3 | нет | остаток совпадает с NV-1 (отсутствующее expected-old предусловие repoint'а) | Даёт ли per-ref-путь CAS ту атомарность/видимость, которая нужна MVCC, раз `supportTransaction` теперь принимает CAS через `supportsTransactionalMutableFiles`? — Даёт: никакого «sidecar» больше нет, `txn_version.txt` — обычная запись дерева манифеста, публикуемая ОДНОЙ ref-log транзакцией, и view инвалидируется сразу после неё. |
| NV-14 | частично подтверждено | P3 | нет | не отслеживается; смежное и уже адъюдицированное — T11, `docs/superpowers/cas/opus-review-tr… | Верно ли, что fan-out-сайты не разворачивают прокси, и оправдывает ли это доктрина хелпера? — Сайты действительно не разворачивают (и их не три, а четыре), доктрина их НЕ покрывает (это не query paths), но последствия от безобидных до одного ослабления защиты в `SYSTEM DROP REPLICA … FROM ZKPATH`. |

---

# Детали {#details}

## B1 (исправлено, P3) {#b1}

**Диалект больше не включается на весь клиент по `http_client = gcp_oauth`, и заявленная регрессия Iceberg `version-hint.text` закрыта конкретно — её запрос идёт как `Default` и проходит по upstream-пути байт-в-байт.**

**Заявлено (обзор, B1).** `ClientFactory::create` (`src/IO/S3/Client.cpp:1313` на момент обзора) ставил
`gcs_conditional_dialect = true` для всего клиента при `http_client = "gcp_oauth"` — значения, которое
предшествует CAS (upstream PR #96975, 26.2) и используется обычной табличной функцией `s3` и named
collections. Четыре следствия: (1) прослеженная регрессия — Iceberg отправляет реальный hex-ETag как
`If-Match` в оптимистичном обновлении `version-hint.text` (`Iceberg/Utils.cpp:365`), диалект бросает
`LOGICAL_ERROR` на любом не-числовом `If-Match`, а `catch (...)` Iceberg его глотает, поэтому обновления
`version-hint` на Iceberg + GCS + `gcp_oauth` молча и навсегда перестают работать; (2) на каждом запросе
срезаются `authorization`/`x-amz-date`/`x-amz-content-sha256`/`x-amz-security-token`/`x-amz-api-version`,
остальные `x-amz-*` переименовываются в `x-goog-*`, а в ответе `ETag` перетирается закавыченным
`x-goog-generation`; (3) доки описывали более узкий охват, чем код; (4) нулевое e2e-покрытие,
`PocoHTTPClientGCSHMAC` не упоминался ни в одном тестовом файле диффа.

**Дубликат по существу основной части.** Гейтинг диалекта — это `fable-review-triage.md` **M9**
(`docs/superpowers/cas/fable-review-triage.md:342` {#m9}), статус там **исправлено**, фиксящий коммит
`faab6678d8f` «Isolate GCS generation adaptation to explicitly marked CAS requests» (21.08). Ту
адъюдикацию я не переделываю. Ниже перепроверено только то, чего в M9 не было: **закрыто ли следствие
именно про Iceberg**, а не только гейтинг флага.

**Что на HEAD (`5917b0f2bf9`).**

1. Клиент-широкого флага нет: `grep -rn "gcs_conditional_dialect\|usesGcsConditionalDialect" src/` даёт
   ноль вхождений.

2. Диалект применяется только к запросу, помеченному `NativeConditional`:
   - `src/IO/S3/PocoHTTPClient.cpp:911-915` (`PocoHTTPClientGCPOAuth::makeRequestInternal`), с
     комментарием `:908-910`: «A `Default` request keeps pre-CAS upstream behaviour: the Bearer token
     replaces `Authorization` and every other SDK header is left alone. Only a `NativeConditional`
     request acquires generation semantics and has its stale AWS signing artifacts removed.» —
     `if (isNativeConditionalRequest(request)) { applyGcsConditionalDialectToRequest(request);
     prepareGcsRequestForOAuthAuthentication(request); }`.
   - Ответная сторона: `src/IO/S3/PocoHTTPClient.cpp:758-761` — подмена `ETag` только
     `if (isNativeConditionalRequest(request)) applyGcsConditionalDialectToResponse(...)`.
   - Контракт зафиксирован в заголовке: `src/IO/S3/GCSConditionalDialect.h:11-13` («ONLY for a request
     marked `NativeConditional`, so ordinary traffic through the same client keeps upstream AWS
     semantics») и `:45-47` («A `Default` response is never passed here and so keeps its upstream ETag
     and headers byte-for-byte»).
   - Бит выводится заново на каждой попытке SDK: `src/IO/S3/Client.cpp:1011-1018` — комментарий
     «Re-derived on every attempt: a retry or redirect discards the old HTTP request and builds a fresh
     one (see `AWSClient::AttemptExhaustively`), so the bit cannot be left to survive on it», и
     `extended_http_request->setNativeConditional(wrapper && wrapper->isNativeConditional() &&
     supportsGcsNativeConditionalRequests())`.

3. **Следствие про Iceberg закрыто конкретно, а не только «через гейтинг».** Путь Iceberg прослежен
   до конца и он `Default`:
   - `src/Storages/ObjectStorage/DataLakes/Iceberg/Utils.cpp:269,274` — записывается только
     `write_settings.object_storage_write_if_match = write_if_match;`.
   - `object_storage_request_mode` в этих настройках Iceberg НЕ трогает; дефолт —
     `src/IO/WriteSettings.h:89` `ObjectStorageRequestMode object_storage_request_mode =
     ObjectStorageRequestMode::Default;`.
   - `src/IO/WriteBufferFromS3.cpp:656-657` ставит `req.SetIfMatch(...)`, а строкой ниже, `:663`,
     `req.setNativeConditional(write_settings.object_storage_request_mode ==
     ObjectStorageRequestMode::NativeConditional)` — то есть для Iceberg бит выставляется в `false`
     (то же в `:746-747`/`:756`).
   - Следовательно `applyGcsConditionalDialectToRequest` для этого запроса не вызывается, и
     `LOGICAL_ERROR` на не-числовом `If-Match` (`src/IO/S3/GCSConditionalDialect.cpp:151`, «GCS
     native-conditional request: If-Match value '{}' is not a generation number…») недостижим по
     Iceberg-пути. `ETag` ответа тоже не перетирается. Единственный CAS-сайт, который выставляет
     режим, — `Backend/CasObjectStorageBackend.cpp:839` `ws.object_storage_request_mode =
     ObjectStorageRequestMode::NativeConditional;`.

4. Пункт (2) обзора (безусловные strip/rename и подмена `ETag`) закрыт тем же гейтом — оба цикла теперь
   под `isNativeConditionalRequest`. Пункт (3) (расхождение доков) закрыт `b4f34cfb92c` «Correct the GCS
   authentication prose after the per-request dialect flip».

5. Пункт (4) (нулевое покрытие) закрыт существенно: `8562e4c1690` «Pin non-CAS GCS authentication
   behavior» (21.08) добавил именно характеризацию `Default`-путей — `src/IO/S3/tests/gtest_aws_s3_client.cpp`
   (+355), `gtest_goog4_signer.cpp` (+59), а также мок-GCS e2e:
   `tests/integration/test_storage_gcp_auth/gcs_mocks/echo.py` (+285) и `test.py` (+134), по сообщению
   коммита — «`gcp_oauth` end-to-end over GET/HEAD/LIST/DELETE/multipart-sized writes». Плюс
   `src/IO/S3/tests/gtest_gcs_conditional_dialect.cpp` — 29 тестов, включая `NonNumericIfMatchThrows`,
   `AuthenticationPreparationLeavesConditionsAlone`, `RequestMetadataDoesNotTouchOtherAmzHeaders`,
   `Goog4PreparationLeavesNonAmzHeadersAlone`, `ResponseWithoutGenerationKeepsETag`.
   Ранее `5173ca20552` добавил death-тесты на LOGICAL_ERROR-гварды диалекта.

**Цепочка коммитов, закрывшая пункт:** `9601ac360f0` (типизированное состояние `NativeConditional`,
`ObjectStorageRequestMode`, `ExtendedRequest::setNativeConditional`) → `378472fb5cf` → `5d7f26274cb`
(метаданные и delete через generations) → `9b887ac8886` (записи) → `faab6678d8f` (**фиксящий**:
атомарное переключение адаптера на per-request гейт, удаление флага) → `b4f34cfb92c` (проза) →
`8562e4c1690` (пины не-CAS поведения) → `10e97f99b0c`, `576e5511c22`, `2ca5677d588` (отказ от смены
диалекта при перезагрузке диска).

**Что осталось (почему P3, а не «—»), в точности как в M9:**
1. Живая валидация на реальном GCS: план `docs/superpowers/plans/2026-08-20-cas-gcs-request-isolation.md`
   (Task 9) не отмечен закрытым; BACKLOG держит это открытым —
   `BACKLOG/formats-and-storage.md:28` «`gcp_oauth` dialect probe validation against live GCS (ADC
   creds)», и `:27` «must follow completion of the current GCS Task 9 gate»; предрелизный чек —
   `final-checks-todo.md:113-117` {#gcs-request-isolation}.
2. Релиз-нота про новую строгость `gcs_hmac`: `prepareGcsRequestForGoog4Authentication` выполняется для
   КАЖДОГО запроса этого клиента (`src/IO/S3/PocoHTTPClient.cpp:1021-1023`, комментарий
   `GCSConditionalDialect.h:31-39`), и любой `x-amz-*` без правила даёт `BAD_ARGUMENTS` вместо молчаливого
   переименования. Это только `http_client = gcs_hmac` (CAS требует именно его), обычный `gcp_oauth`
   не задет, но changelog-строки всё ещё нет.
3. Задокументированное побочное следствие (`GCSConditionalDialect.h:48-51`): `Default`-чтение атрибутов
   CAS-объекта отдаёт пустую карту метаданных, а не ошибку. Осознанно.

Никакой части исходного пункта, достижимой на HEAD, не осталось: заявленная регрессия Iceberg не
воспроизводится, охват переписывания сужен до помеченных CAS-запросов, покрытие добавлено.

## B2 (частично, P2) {#b2}

**Головная половина подтверждена дословно — cap жив, `NOT_IMPLEMENTED` классифицируется как детерминированный локальный отказ и пробивает ретрай-петлю, ни Compose, ни fail-closed отказа монтирования нет; вторая половина («cap = `min_upload_part_size`») на HEAD НЕ подтверждается — эта связка удалена, хотя потолок памяти остался по другому механизму.**

**Заявлено (обзор, B2).** `conditionalWriteSettings()` для generation-token store ставит
`s3_force_single_part_upload = true` и проецирует cap на **оба** — `max_single_part_upload_size` и
`min_upload_part_size`. Часть, чей крупнейший столбец `.bin` превышает cap, входит в
`createMultipartUpload`, который бросает `NOT_IMPLEMENTED`; `isDeterministicLocalFailure` относит это к
детерминированному локальному отказу и пробрасывает без ретрая; ограниченная петля `uploadBlobDetached`
перебрасывает всё, что не `ABORTED`. Последовательность: мерж рождает часть с >1 GiB столбцом →
`stageManifest`/`precommitAdd` проходят → поток blob'а перешагивает cap → `NOT_IMPLEMENTED` → коммит
падает → rollback → **мерж перепланируется и воспроизводит идентичный детерминированный отказ на каждой
попытке**; части накапливаются без пути самоисцеления. Второй обрыв: так как cap ещё и
`min_upload_part_size`, пиковая память на одну условную запись — `min(blob_size, cap)`, при
`cas_blob_upload_pool_size` (16) — до ~16 GiB. Предложено: реализовать путь, который назван в самом
тексте броска (безусловный multipart во временный ключ + условный `Compose` для публикации); либо, как
промежуточная fail-closed мера, отказывать в **монтировании**, когда настроенный максимум размера файла
части может превысить cap, и задокументировать потолок; независимо — расцепить
`min_upload_part_size` и `max_single_part_upload_size`.

**Что на HEAD (`5917b0f2bf9`).**

**Подтверждено — головная половина, целиком.**

1. Настройка переименована, cap на месте, дефолт тот же:
   `ContentAddressedSettings.cpp:93` — `DECLARE(UInt64, gcs_max_token_producing_put_bytes, 1ULL << 30,
   "GCS single-PUT budget for every token-producing write, conditional or not (generation-token stores
   only)", 0)`. Старое имя `gcs_max_conditional_put_bytes` теперь отвергается как неизвестное
   (закреплено тестом `src/Disks/tests/gtest_cas_settings.cpp:75`). Проводка:
   `ContentAddressedMetadataStorage.h:616`, `.cpp:296`, `:694`.

2. Форсирование single-part и проекция cap живы, и охват стал ШИРЕ, чем в обзоре — теперь это не только
   условная запись, а любая «token-producing»:
   `Backend/CasObjectStorageBackend.cpp:836-846` —
   `WriteSettings ObjectStorageBackend::tokenProducingWriteSettings() const { WriteSettings ws;
   ws.object_storage_request_mode = ObjectStorageRequestMode::NativeConditional; if (native_token_type ==
   TokenType::Generation) { ws.s3_force_single_part_upload = true;
   ws.s3_single_part_upload_max_bytes_override = token_producing_single_put_cap; } return ws; }`, и
   `conditionalWriteSettings()` (`:853-855`) начинается с `WriteSettings ws =
   tokenProducingWriteSettings();`. ETag-диалект не затронут.

3. Бросок ровно там, где заявлено, и его текст сам называет нереализованный путь:
   `src/IO/WriteBufferFromS3.cpp:407-416` — `void WriteBufferFromS3::createMultipartUpload() { if
   (write_settings.s3_force_single_part_upload) throw Exception(ErrorCodes::NOT_IMPLEMENTED, "A
   token-producing write would start a MULTIPART upload… The single-PUT budget is governed by the disk
   setting gcs_max_token_producing_put_bytes; **the production-grade path for bigger blobs
   (unconditional multipart to a temp key + conditional Compose) is not implemented yet.**"`.

4. Классификация не изменилась:
   `Backend/CasRequestControl.cpp:114-118` — `bool isDeterministicLocalFailure(int code) { return code ==
   ErrorCodes::LOGICAL_ERROR || code == ErrorCodes::NOT_IMPLEMENTED || code == ErrorCodes::BAD_ARGUMENTS
   || code == ErrorCodes::CORRUPTED_DATA; }`, и контроллер на таком коде RETHROW'ит без ретрая
   (`CasRequestControl.h:523`, `:580`; сайты `:452`, `:527`, `:608`, `:699`).

5. Петля `uploadBlobDetached` по-прежнему перебрасывает всё, кроме `ABORTED`:
   `Pool/CasPartWriteTxn.cpp:237-259` — `constexpr int max_attempts = 8; for (…) { try { …
   uploadFromSource(…) … } catch (const Exception & e) { if (e.code() != ErrorCodes::ABORTED || attempt +
   1 == max_attempts) throw; } }`. То есть `NOT_IMPLEMENTED` выходит с первой попытки, детерминированно
   и на каждом перепланировании мержа. Заявленная «мержи встают навечно» — верна.

6. **Ни Compose, ни fail-closed отказа монтирования нет.** `grep` по `Compose` в поддереве
   `ContentAddressed/` пуст; `token_producing_single_put_cap` используется ровно в двух местах —
   конструкторе (`Backend/CasObjectStorageBackend.cpp:42,45`) и `:843`; в `Backend/CasProbe.cpp` и
   `Pool/CasPool.cpp` (пути монтирования) упоминаний cap нет вовсе, то есть предложенная промежуточная
   мера «отказать в монтировании» не реализована. Ни S3-native staging, ни работа по изоляции
   GCS-запросов пути для крупных blob'ов не добавили: staging-путь тоже идёт через
   `conditionalWriteSettings()` (`Backend/CasObjectStorageBackend.cpp:1118`).

**Не подтвердилось — вторая половина в заявленной форме.** Связки cap ↔ `min_upload_part_size` на HEAD
нет: `grep -rn "min_upload_part_size\|s3_min_upload_part_size"` по всему поддереву
`ContentAddressed/` даёт **ноль** вхождений; cap проецируется только на
`s3_single_part_upload_max_bytes_override` (`:843`). Так что предложение «расцепить
`min_upload_part_size` и `max_single_part_upload_size`» уже неактуально — расцеплено.
Однако СЛЕДСТВИЕ про память живо по другому механизму: `s3_force_single_part_upload` означает, что тело
обязано уйти одним PUT, то есть буферизуется целиком, и потолок — `min(blob_size, cap)` на одну
запись при `cas_blob_upload_pool_size` = 16 по умолчанию (`src/Core/ServerSettings.cpp:152`). BACKLOG
формулирует это точнее самого обзора: «**Raising the cap does not help**, it just moves the memory
ceiling: one part means one RAM buffer» (`BACKLOG.md:441-442`). Так что цифра ~16 GiB верна, а
приписанная ей причина — нет.

**Фиксящего коммита нет.** Изменения после обзора по этой теме — переименование настройки и РАСШИРЕНИЕ
охвата cap с «условных» на «token-producing» записи (`Backend/CasObjectStorageBackend.cpp:826-835`,
`:1172-1179`), то есть ограничение стало строже, а не слабее.

**Что нашлось сверх обзора — BACKLOG в этой части УСТАРЕЛ, и это надо поправить.**
`BACKLOG.md:424-430` утверждает: «**SCOPE NARROWED (2026-08-04): the resurrect path no longer has this
problem.** The condemned-blob resurrect is now an UNCONDITIONAL write (`Backend::resurrect`): on remote
object storage it streams and takes multipart -- size-unlimited on GCS too». На HEAD это неверно:
`Backend/CasObjectStorageBackend.cpp:1172-1179` прямо говорит обратное — «This write carries no
precondition, but it is still routed through `tokenProducingWriteSettings` rather than plain
`WriteSettings`: it is a token-producing write…, so **on a generation-token store (GCS) it is bound by
the same single-PUT cap a CONDITIONAL write is** -- GCS drops preconditions on multipart completion
regardless of whether one was ever set», и код действительно вызывает
`object_storage->writeObject(…, tokenProducingWriteSettings())` (`:1181-1183`). То есть на GCS
resurrect тела выше cap так же невозможен, как и create, и «сужение охвата» из BACKLOG отыграно назад
последующей работой. Соответственно и утверждение BACKLOG «this item is now only about creating a blob
larger than the cap on GCS» пора вернуть к более широкой формулировке.

**Почему P2, а не P1.** Отказ громкий и fail-closed (`NOT_IMPLEMENTED` до первого multipart-запроса), без
порчи и без потери данных — это ровно то, что предписывает правило «loud failure ≫ silent corruption»
(та же логика зафиксирована в `2031-triage.md` вокруг строк 1284-1295). Затронут только generation-диалект,
то есть GCS; на ETag-совместимых хранилищах forcing не включается вовсе. При этом это НЕ «не подтвердилось»:
на заявленном как поддерживаемый бэкенде мерж части с >1 GiB столбцом не имеет пути завершения, и части
накапливаются до вмешательства оператора — реальный дефект доступности.

**Что осталось / как закрывать.** Дизайн-решение, и BACKLOG прямо это фиксирует
(`BACKLOG.md:457-462`): «whether `Compose` is even the right primitive; whether the temp-key debris is
acceptable…; whether GCS should instead be documented as supporting CAS only below a stated blob size…
The answer may legitimately be "cap it, document it, and refuse bigger blobs" — **that is a design
decision, and it has not been made**». Минимум, который стоит сделать до релиза независимо от выбора
пути (и чего сейчас нет): (1) заявить потолок в пользовательской документации GCS-бэкенда явно как
ограничение поддержки, а не только как строку настройки в таблице
(`docs/en/antalya/cas/configuration.md:97`, `architecture/blob-protocol.md:228` описывают настройку, но
не следствие «часть с большим столбцом не смержится»); (2) поправить устаревшее «SCOPE NARROWED» в
`BACKLOG.md:424-430` по факту кода. Промежуточный fail-closed отказ монтирования, предложенный обзором,
остаётся вариантом, но он слабее: настроенный максимум размера файла части не является верхней оценкой
размера столбца, поэтому такая проверка на монтировании не была бы ни необходимой, ни достаточной.

## B3 (подтверждено, P1) {#b3}

**Форма подтверждена дословно на HEAD и достижима: сильный `const ContextPtr context` (`:595`) копируется в оба синка, `Context::getContentAddressedLog` безусловно разыменовывает `shared`, а `resetSharedContext()` обнуляет его до выхода последнего CAS-события — прощального `MountRelease` из `~Pool` на detached-нити (B4).**

**Заявлено (обзор, B3).** Фабрика передаёт `Context::getGlobalContextInstance()` в
`ContentAddressedMetadataStorage`, который хранит его **сильным** `const ContextPtr`. Оба синка событий
копируют его в `std::function`, принадлежащий `Cas::Pool`, и каждое событие зовёт
`ctx->getContentAddressedLog()` / `...GarbageCollectionLog()`, а те начинаются с
`SharedLockGuard lock(shared->mutex)` — безусловного разыменования сырого `ContextSharedPart * shared`.
`Server.cpp` зовёт `global_context->resetSharedContext()` (обнуляя `shared`) непосредственно перед
`global_context.reset()`. Любое CAS-событие после этой точки — нулевое разыменование, а не мягкий no-op.
Достижимо через **B4**. Плюс: это единственный долгоживущий сильный `ContextPtr`-член в `src/Disks`, все
соседи используют слабый паттерн `WithContext`; и сильная ссылка означает, что `global_context.reset()`
больше не разрушает `Context`, противореча документированному инварианту на том же call-site.

**Что на HEAD (`5917b0f2bf9`) — все пять звеньев цепочки подтверждены.**

1. Член по-прежнему сильный:
   `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:595`
   — `const ContextPtr context;`. Заголовок сам называет захват:
   `:776-777` («…`ContextPtr`, converts the POD `GcRoundLogRecord` into a
   `ContentAddressedGarbageCollectionLogElement`…»), `:781-783` («Captures the `ContextPtr`…»).

2. Оба синка копируют его по значению в лямбду и зовут через него на каждом событии:
   - GC-синк: `ContentAddressedMetadataStorage.cpp:486-497` — `auto ctx = context;` (`:490`),
     `return [ctx, disk](const Cas::GcRoundLogRecord & r) { auto log = ctx->getContentAddressedGarbageCollectionLog(); …}`
     (`:494-497`).
   - Синк аудита: `:565-575` — `auto ctx = context;`, `return [ctx, disk](Cas::CasEvent ev) { auto log =
     ctx->getContentAddressedLog(); …}` (`:574-575`).
   Ни одного `weak_ptr`, ни одного захвата самого `shared_ptr<SystemLog>` вперёд.

3. Разыменование безусловное, без проверки `shared`:
   `src/Interpreters/Context.cpp:6336-6342` — `std::shared_ptr<ContentAddressedGarbageCollectionLog>
   Context::getContentAddressedGarbageCollectionLog() const { SharedLockGuard lock(shared->mutex); … }`;
   `:6344-6350` — то же для `getContentAddressedLog`. Никакого `mutex_shared_context`, никакой
   nullptr-проверки — в отличие от `ContextData::tryGetConfig` (`:1352-1356`), который специально берёт
   `mutex_shared_context` и пишет `return shared ? … : nullptr`.

4. Обнуление и порядок на шатдауне:
   `src/Interpreters/Context.cpp:1346-1350` — `void ContextData::resetSharedContext() {
   std::lock_guard<std::mutex> lock(mutex_shared_context); shared = nullptr; }`;
   `programs/server/Server.cpp:1644-1649` — комментарий «**Explicitly destroy Context** … At this moment,
   no one could own shared part of Context.», затем `global_context->resetSharedContext();`,
   `global_context.reset();`, `shared_context.reset();`.

5. Источник ссылки — фабрика:
   `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:231` — `auto global_context =
   Context::getGlobalContextInstance();`, `:240` — `return
   std::make_shared<ContentAddressedMetadataStorage>(…)`.

**Достижимость — да, и это решает P1 против P2.** Требуется CAS-событие ПОСЛЕ `resetSharedContext()`.
Такой путь на HEAD существует ровно тот, что назвал обзор (B4, перепроверено самостоятельно):
- `ContentAddressedMetadataStorage::shutdown()` (`:921-942`) джойнит только нити GC-планировщика
  (`old_scheduler->stop()`, `:940-941`) и сбрасывает `part_access`/`cas_store` под `pointer_mutex`
  (`:928-935`). Никакого дренажа detached-нитей.
- Две detached-нити держат сильную ссылку на `Pool` по значению и НИКЕМ не трекаются:
  `Pool/CasPool.cpp:1489` `auto self = shared_from_this();` → `:1492` `ThreadFromGlobalPool([self, key]…`
  → `:1520` `.detach();` (`reportImpossibleInterference`); и `Pool/CasRefLedger.cpp:4003`
  `auto owner = pin_owner();` → `:4006` `ThreadFromGlobalPool([owner, this, ns, rt]…` → `:4017`
  `.detach();` (`dispatchSnapshotPublisher`). Второй — не аномальный, а штатный путь публикации
  снимка на обычной ref-мутации, поэтому нить в полёте на момент шатдауна вполне обыденна.
  (`2031-triage.md` CAS-052 {#cas-052} разбирал ДРУГОЕ утверждение про эти же сайты — `bad_weak_ptr` —
  и признал его недостижимым, но при этом прямо ПОДТВЕРДИЛ механику, которая нужна здесь: «Лямбда
  захватывает `self` ПО ЗНАЧЕНИЮ … то есть поток держит собственный `shared_ptr` и продлевает жизнь
  пула на всё своё время».)
- Если такая нить — последний владелец, `~Pool` исполняется на ней после возврата из `shutdown()`, и
  `~Pool` (`Pool/CasPool.cpp:872-900`) доходит до `mount_runtime.finishTeardown(drained)` (`:899`) →
  `CasMountRuntime::finishTeardown` (`Pool/CasMountRuntime.cpp:505-542`) → `mount_keeper->stop()`
  (`:517`) → графful-release, который **безусловно** эмитит событие:
  `Pool/CasServerRoot.cpp:1270-1271` — `emitMountEvent(event_sink, CasEventType::MountRelease, srid,
  "farewell", nullptr, "graceful release — lease stamped already-expired, watermark farewell folded in");`.
  `emitMountEvent` (`:326-330`) защищён только `if (!sink) return;` — про валидность `Context` он ничего
  не знает.
- Итог: `ctx->getContentAddressedLog()` на `shared == nullptr` → `SharedLockGuard lock(shared->mutex)` →
  SIGSEGV в окне между `resetSharedContext()` и завершением процесса. Обратите внимание: durable-запись
  прощания (`recordWrite(seq + 1, res.token)`, `CasServerRoot.cpp:1272`) идёт ПОСЛЕ эмита, то есть падение
  здесь дополнительно съедает и её.

**Заявление «сильная ссылка ломает инвариант `global_context.reset()`» — тоже верно.** `context` —
`shared_ptr<const Context>`, поэтому пока CAS-диск жив в реестре (а он живёт до конца процесса, см. M7 в
`fable-review-triage.md:307`), `global_context.reset()` не разрушает `Context`; комментарий на
`Server.cpp:1644-1646` «no one could own shared part of Context» продолжает описывать неверное
состояние.

**Заявление «единственный долгоживущий сильный `ContextPtr` в `src/Disks`» — верно.**
`grep` по `src/Disks/` на объявления `ContextPtr`-членов даёт три сайта:
`ContentAddressedMetadataStorage.h:595`, `src/Disks/IO/ReadBufferFromWebServer.h:46` (короткоживущий
read-буфер, живёт в пределах чтения) и `src/Disks/DiskFromAST.cpp:141` (локальная структура визитора).
Слабый паттерн `WithContext` в `src/Disks/` используют `DiskLocalCheckThread.{h,cpp}` и
`WebObjectStorage.{h,cpp}`. `ContextWeakPtr` в `src/Disks/` не встречается вовсе.

**Фиксящего коммита нет.** `git log 056488b47a0..HEAD -S "ContextWeakPtr"` — пусто;
`git log 056488b47a0..HEAD -S "const ContextPtr context;" -- src/Disks/` даёт только `7133ba900b1` («CA
wiring (M-W Task 2)»), который ДОБАВИЛ этот член задолго до обзора, а не поправил его.

**Почему P1 и pre-release `да`.** Это разыменование нулевого указателя (не исключение, не логическая
ошибка) на пути штатного шатдауна сервера, достижимое штатной фоновой публикацией снимка; следствие —
SIGSEGV/«Server died» с непустым exit-кодом и core dump, плюс потеря прощального маркера, от которого
зависит выбор пути восстановления преемником. Это ровно та категория, которую политика «🟢 нет известных
красных» не разрешает нести в релиз.

**Что осталось / как закрывать.** Ровно как предложил обзор, и предпочтительно вторым способом, потому
что он убирает повторный вход в `Context` вообще: захватить `shared_ptr` на сам системный лог один раз
(на `startup()` или в момент создания синка) и держать в лямбде его, а не `ContextPtr`. Минимальный
вариант — хранить `ContextWeakPtr` и `lock()` на каждом использовании (тогда после
`resetSharedContext()` синк станет тихим no-op'ом вместо падения). Оба варианта локальны для
`ContentAddressedMetadataStorage` и не трогают общий upstream-код `Context`. Пункт нужно ЗАВЕСТИ в
`BACKLOG/mounts-and-lifecycle.md` (там же, где residual'ы rev.8) и в `final-checks-todo.md` —
сейчас он не отслеживается нигде. Смежно: полностью снять достижимость можно и со стороны **B4**
(дренаж detached-нитей до `cas_store.reset()`), но оставлять при этом безусловное разыменование
неправильно — это два независимых дефекта, и второй эшелон защиты нужен здесь.

## B4 (подтверждено, P1) {#b4}

**Оба заявленных detached-дispatch'а живы, по-прежнему не трекаются и держат сильную ссылку на `Pool`; `shutdown()` их не дренирует, поэтому `~Pool` с прощальной долговечной записью и эмитом события штатно может исполниться после того, как object storage уже погашен, а `Context` обнулён (B3). rev.8 закрыл соседнюю половину (само-выход GC-нитей), но не эту.**

**Заявлено (обзор, B4).** `shutdown()` джойнит только две нити GC-планировщика, затем сбрасывает
`cas_store`/`part_access`/`gc_scheduler`. Два других пути запускают **detached**
`ThreadFromGlobalPool`, каждая держит сильную ссылку на `Pool` (`shared_from_this()` в
`reportImpossibleInterference`, `pin_owner()` в `dispatchSnapshotPublisher`), и они никогда не
трекаются и не джойнятся. Если такая нить окажется последним владельцем, `~Pool` исполняется **на ней,
после возврата из `shutdown()`**. `~Pool` не тихий: `drainRefLanesForShutdown` + `finishTeardown` →
`MountLeaseKeeper::stop()` → терминальный release, который делает I/O по object storage и безусловно
эмитит `MountRelease`/«farewell» через синк событий. К этому моменту `DiskObjectStorage::shutdown()` уже
погасил object storage, а сервер ушёл в `resetSharedContext()` — значит прощальная запись
(корректностно значимая: по собственному комментарию кода незаслуженное или отсутствующее прощание
меняет восстановление преемника) исполняется против мёртвого хранилища, а её событие попадает в нулевое
окно B3. Предложено: трекать detached-дispatch'и (счётчик + condvar, либо общий `ThreadPool`, чей
`wait()` вызывается) и дренировать их до `cas_store.reset()`; как минимум сделать `~Pool` fail-soft вне
нити шатдауна.

**Что на HEAD (`5917b0f2bf9`).** Подтверждается каждое звено.

1. `shutdown()` дренирует только GC:
   `ContentAddressedMetadataStorage.cpp:921-942` — `std::lock_guard round_lock(gc_scheduler_mutex);`
   (`:924`), `shutdown_called = true;`, затем под `pointer_mutex` (`:928`) `old_scheduler =
   std::move(gc_scheduler); gc_scheduler.reset(); part_access.reset(); … cas_store.reset();`
   (`:929-935`), и вне лока `if (old_scheduler) old_scheduler->stop();` (`:940-941`) с комментарием
   «`stop` joins the background threads». Никакого ожидания detached-нитей — ни счётчика, ни condvar, ни
   `ThreadPool::wait()`.

2. Ровно два `.detach()` во всём CAS-поддереве, оба — заявленные, оба держат `Pool` по значению:
   - `Pool/CasPool.cpp:1489` — `auto self = shared_from_this();` (вне `try`), `:1492`
     `ThreadFromGlobalPool([self, key] { setThreadName(ThreadName::CAS_ANOMALY_DIAG); … })`, `:1520`
     `.detach();`. Комментарий `:1486-1488` сам это фиксирует: «`shared_from_this()` keeps the Pool
     alive for the thread's lifetime (mirrors `maybeScheduleSnapshotPublish`'s dispatch)».
   - `Pool/CasRefLedger.cpp:4003` — `auto owner = pin_owner();`, `:4006`
     `ThreadFromGlobalPool([owner, this, ns, rt] { setThreadName(ThreadName::CAS_REF_SNAPSHOT_PUBLISH); … })`,
     `:4017` `.detach();`. Комментарий `:3994-3995`: «`pin_owner()` (the Pool's `shared_from_this`) keeps
     the Pool -- and hence this ledger member -- alive for the thread's lifetime».
   `grep -rn "\.detach()"` по всему поддереву `ContentAddressed/` даёт ровно эти две строки.
   `2031-triage.md` {#cas-052} независимо ПОДТВЕРДИЛ ту же механику («Лямбда захватывает `self` ПО
   ЗНАЧЕНИЮ … поток держит собственный `shared_ptr` и продлевает жизнь пула на всё своё время»), хотя
   разбирал другое утверждение (`bad_weak_ptr` — недостижимо).

3. Дренаж внутри `~Pool` до публикаторов не достаёт:
   `Pool/CasRefLedger.cpp:1854` `drainRefLanesForShutdown` защёлкивает `shutting_down` (`:1860`) и
   ждёт `rt->cv.wait_until(lk, deadline)` (`:1885`) — то есть **лейны аппенда**, а не публикаторов.
   Механизм ожидания публикаторов в файле СУЩЕСТВУЕТ (`publish_settle_cv.wait(slock, [&]{ return
   rt->pending_snapshot_publishes.load(…) == 0; })`, `:1712-1713`), но вызывается он из
   `enforceRefTableCacheBudget` и `dropNamespace` — ни из `shutdown()`, ни из `~Pool`. Значит
   публикатор в полёте штатно переживает и `shutdown()`, и сброс `cas_store`.

4. Публикатор — не аномальный, а горячий путь. `dispatchSnapshotPublisher` вызывается из штатной
   ref-мутации (`admitSnapshotPublishUnderStateLock` инкрементит счётчик на `:3987`), поэтому «нить в
   полёте на момент шатдауна» — обыденное состояние, а не экзотика. Это и делает пункт достижимым, в
   отличие от диагностической нити аномалий.

5. `~Pool` не тихий — дословно как заявлено:
   `Pool/CasPool.cpp:872-900` — `mount_runtime.stopRemountThread();` (`:879`), затем
   `ref_ledger.drainRefLanesForShutdown(config.cas_request_budget.attempt_timeout_ms +
   config.cas_request_budget.lease_safety_margin_ms)` (`:891-892`) и
   `mount_runtime.finishTeardown(drained)` (`:899`). Комментарий `:886-890` сам объявляет прощание
   корректностно значимым: «Writing it without an actual drain would be a protocol-safety bug: an
   uncertain PUT this incarnation is still resolving could land AFTER the successor already reclaimed
   and started mutating».
   `Pool/CasMountRuntime.cpp:505-542` `finishTeardown` → `mount_keeper->stop()` (`:517`) на чистом
   дренаже.

6. Прощальный эмит по-прежнему безусловно идёт через `Context`-синк (сцепка с B3):
   `Pool/CasServerRoot.cpp:1270-1271` — `emitMountEvent(event_sink, CasEventType::MountRelease, srid,
   "farewell", nullptr, "graceful release — lease stamped already-expired, watermark farewell folded
   in");`, и сама долговечная запись идёт СТРОКОЙ НИЖЕ — `:1272` `recordWrite(seq + 1, res.token);`.
   `emitMountEvent` (`:326-330`) защищён только `if (!sink) return;`.

7. Порядок «object storage уже погашен» подтверждается:
   `src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:504-516` — `metadata_storage->shutdown();`
   (`:511`), и только ПОСЛЕ него `for (…) object_storages->takePointingTo(location)->shutdown();`
   (`:512-513`). Раз `~Pool` может выполниться позже возврата из `metadata_storage->shutdown()`,
   он застаёт погашенный object storage — то есть терминальная запись прощания идёт против мёртвого
   хранилища.

8. Fail-soft off-shutdown-thread НЕ сделан. В `finishTeardown` `try/catch` обёрнут только
   `mount_keeper->stop()` (`Pool/CasMountRuntime.cpp:515-522`), а `stopRemountThread()` (`:513` в
   `~Pool`) и `drainRefLanesForShutdown` (`:891`) — нет; `~Pool` не `noexcept`-безопасен. Тот же
   теоретический класс уже зафиксирован в `2031-triage.md` {#cas-018} («Ни один из вызовов не
   `noexcept`, и внутри `finishTeardown` есть аллоцирующие `LOG_WARNING` … остаётся тот же
   теоретический класс», P3) — но там он рассматривался как аллокации под лимитом памяти, а не как
   `~Pool` на чужой нити после шатдауна.

**Что закрыл rev.8 (это соседняя половина, не эта).** Раунд rev.8 действительно работал в этой области,
но по другой оси — само-выход фоновых нитей на терминальном пуле:
`BACKLOG/mounts-and-lifecycle.md:33-35` — «G1-G5 resolved this round (isolation fix, throw-not-abort, GC
self-exit on `Vanished`/`IdentityLost`, generic-code correctness, FSCK-on-running advisory)», и
`fable-review-triage.md` {#m7} подтверждает механику: `Gc/CasGcScheduler.cpp:365-378` — heartbeat-цикл
проверяет `isVanished()`/`vanishedIntentPublished()`/`IdentityLost` и возвращается. То есть rev.8 сделал
так, что GC-нить не бьётся об удалённый префикс пула, а `~Pool`-на-чужой-нити не тронул вовсе.
Обратная сторона того же раунда — residual (b) в том же разделе
(`BACKLOG/mounts-and-lifecycle.md:46-49`): доступ к нулевому пулу теперь FAIL-LOUD (`INVALID_STATE`),
что делает окно шатдауна более, а не менее шумным.

**Фиксящего коммита нет.** `git log 056488b47a0..HEAD -S "reportImpossibleInterference"`,
`-S "dispatchSnapshotPublisher"`, `-S ".detach()"` по поддереву `ContentAddressed/` не дают ни одного
коммита ПОСЛЕ обзора: все попадания (`0f895c6f40d`, `d8b401ff035`, `636d0445791`, `61de04b0389`,
`f971c0c27d9`, `a5062c3f427`, `b271bf65a02`) старше `056488b47a0`.

**Почему P1 и pre-release `да`, хотя `fable` M8 (тот же деструктор) получил P3.** M8 — про МЕСТО
вызова (`~Pool` под `pointer_mutex` блокирует читателей снимка на ~7 с): ограниченная задержка, без
потери данных, поэтому P3 там оправдан. Здесь речь про МОМЕНТ: `~Pool` исполняется после того, как
(а) object storage погашен — значит корректностно значимая прощальная запись либо не проходит, либо
проходит наполовину, и преемник получает не то состояние, на которое рассчитывает протокол; (б)
`Context` обнулён — значит эмит на `:1270` даёт нулевое разыменование (B3), причём ДО `recordWrite` на
`:1272`. Это не задержка, а порча пути восстановления плюс падение процесса, и достижимо оно штатной
публикацией снимка, а не аномальным путём.

**Что осталось / как закрывать.** Ровно предложение обзора и ровно в предложенном порядке:
1. Трекать оба dispatch'а (счётчик + condvar на `Pool`, либо выделенный `ThreadPool`, чей `wait()`
   вызывается) и дренировать их в `ContentAddressedMetadataStorage::shutdown()` ДО
   `cas_store.reset()` — тогда `~Pool` гарантированно исполняется на нити шатдауна, до погашения
   object storage. Заметим, что нужный примитив в коде уже есть (`pending_snapshot_publishes` +
   `publish_settle_cv`, `Pool/CasRefLedger.cpp:1712-1713`) — его достаточно вызвать из пути шатдауна;
   для диагностической нити аномалий такого счётчика нет, его надо добавить.
2. Независимо — сделать `~Pool` fail-soft (обернуть `stopRemountThread`/`drainRefLanesForShutdown`
   тем же `tryLogCurrentException`, что уже стоит вокруг `mount_keeper->stop()`), чтобы `~Pool` на
   чужой нити не мог завершить процесс через `std::terminate`.
3. Закрыть B3 независимо: дренаж убирает достижимость, но безусловное разыменование `Context` в синке
   должно перестать быть возможным само по себе.
Пункт нужно ЗАВЕСТИ — сейчас он не отслеживается; естественное место —
`BACKLOG/mounts-and-lifecycle.md` рядом с {#disk-lifecycle-rev8-closure}, плюс строка в
`final-checks-todo.md`.

## B5 (подтверждено, P2) {#b5}

**Механика подтверждена дословно и потолок пересчитан на текущих константах — ровно 146 000 мс; стоп-флаг внутри попытки не опрашивается, сон — голый `std::this_thread::sleep_for`, а самомаскирующий комментарий про «bounded to one step + one backend timeout» после rev.8 стоит уже в ДВУХ местах и по-прежнему неверен.**

**Заявлено (обзор, B5).** `stopRemountThread()` защёлкивает `remount_shutting_down`, ставит
`remount_stop`, нотифицирует, затем джойнит. Нить ремоунта проверяет стоп-флаг только *между*
попытками — **внутри** попытки `tryRemountOnce` держит `remount_mutex` и зовёт
`claimMountAwaitingExpiry(..., sleep_ms)`, где `sleep_ms` — голый `std::this_thread::sleep_for`, и
**стоп-флаг не опрашивается нигде в этой петле**. Потолок:
`mountObservationThresholdMs(ttl, poll) = ttl + ttl/20 + poll` на окно × (`kMaxObservationRestarts` + 1
= 4); при дефолтах (`mount_lease_ttl_ms = 30000`, `mount_renew_period = 10000` ⇒ `poll = 5000`) это
≈ 4 × 36.5 с ≈ **146 с**. Это ровно тот путь, который берётся, когда продление остановилось из-за
*транзиентного* сбоя. Пока блокировка держится, вызывающий удерживает `lifecycle_mutex` и
`gc_scheduler_mutex`, поэтому `SYSTEM CAS FSCK`, `GC STOP/START/RUN` и
`ContentAddressedMetadataStorage::shutdown()` встают в очередь. Самомаскировка: собственный
комментарий `forgetDisk` утверждает *«every join below is bounded to one step + one backend
timeout»* — это не так. Предложено: протащить предикат `should_stop` в `claimMountAwaitingExpiry` (он
уже принимает инъектируемый `sleep_ms_fn`) и проверять его на каждой итерации опроса; исправить
комментарий.

**Что на HEAD (`5917b0f2bf9`) — подтверждается каждое звено.**

1. Стоп-флаг только на границе цикла, и backoff-ожидание — прерываемое (эта половина в порядке):
   `Pool/CasMountRuntime.cpp:461-470` — `while (!remount_stop.load() && !remountTerminal()) { if
   (remount_attempt()) break; std::unique_lock lk(remount_cv_mutex); remount_cv.wait_for(lk,
   std::chrono::milliseconds(backoff_ms), [this] { return remount_stop.load(); }); … }`.
   `stopRemountThread` — `:487-503`: защёлка `remount_shutting_down` под `remount_thread_mutex`
   (`:491-494`), `remount_stop.store(true)` (`:496`), `remount_cv.notify_all()` (`:497`), затем
   `remount_thread.join()` (`:500-502`).

2. `remount_attempt` == `Pool::tryRemountOnce`, и он держит `remount_mutex` на весь свой объём:
   проводка — `Pool/CasPool.cpp:209` `[this] { return tryRemountOnce(); }`; сама функция —
   `Pool/CasPool.cpp:1007-1009`: `bool Pool::tryRemountOnce() { std::lock_guard
   serialize(remount_mutex); …`.

3. Сон внутри попытки — **голый** `sleep_for`, причём на этом пути даже не через инъектируемый
   `waitSleep`: `Pool/CasPool.cpp:1115` — `const auto sleep_ms = [](uint64_t ms) {
   std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };`, и он передаётся в
   `claimMountAwaitingExpiry` на `:1116-1126`. (Стартовый путь монтирования на `:610` хотя бы идёт
   через `mount_runtime.waitSleep`, у которого есть тестовая инъекция `config.wait_sleep_fn`,
   `Pool/CasMountRuntime.cpp:72-78` — но и там никакого стоп-флага нет.)

4. Стоп-предиката в сигнатуре нет вовсе:
   `Pool/CasServerRoot.h:421-429` — `MountClaimResult claimMountAwaitingExpiry(Backend & b, const
   Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch, const
   std::function<uint64_t()> & now_ms_fn, const std::function<uint64_t()> & mono_ms_fn, uint64_t
   ttl_ms, uint64_t poll_interval_ms, const std::function<void(uint64_t)> & sleep_ms_fn, const
   std::function<void(const MountLease &, uint64_t)> & on_wait_start = {}, const CasEventSink & sink =
   {});`. В теле (`Pool/CasServerRoot.cpp:493-568`) — `while (true)` с двумя точками
   `sleep_ms_fn(poll)` (`:544`, `:566`) и ни одной проверки чего-либо про остановку/терминальность.

5. **Потолок пересчитан на текущих константах — цифра обзора точна.**
   - `Pool/CasServerRoot.cpp:488-491` — `uint64_t mountObservationThresholdMs(uint64_t ttl_ms,
     uint64_t cadence_ms) { return ttl_ms + ttl_ms / 20 + cadence_ms; }`;
   - `Pool/CasServerRoot.cpp:485` — `constexpr size_t kMaxObservationRestarts = 3;` (проверки
     `:542`, `:552`), то есть до 4 окон наблюдения;
   - дефолты: `Pool/CasMountRuntime.h:53-54` (и зеркально `Pool/CasPool.h:182-183`) —
     `mount_lease_ttl_ms{30000}`, `mount_renew_period{10000}`;
   - вывод `poll`: `Pool/CasPool.cpp:1018-1019` — `poll_interval_ms = std::max<uint64_t>(1,
     config.mount_renew_period.count() / 2)` = **5000**;
   - `threshold_ms` = 30000 + 30000/20 + 5000 = **36 500 мс**; 4 окна ⇒ **146 000 мс ≈ 2 мин 26 с**.

6. Мьютексы вызывающего — как заявлено:
   `ContentAddressedMetadataStorage::forgetDisk()` берёт `std::lock_guard
   lifecycle(lifecycle_mutex);` и `std::lock_guard round_lock(gc_scheduler_mutex);` и держит их через
   весь `pool->forgetDisk(...)`. Порядок и состав лестницы задокументированы в
   `ContentAddressedMetadataStorage.h:290-291` (`forgetDisk`), `:303` (`gcStop`/`gcStart`), `:313`,
   `:212` (`runFsck` — «Held under `lifecycle_mutex` for the whole scan»); тот же вывод
   независимо зафиксирован в `2031-triage.md` {#cas-049} («`gcStop` … берёт `lifecycle_mutex` +
   `gc_scheduler_mutex` … `forgetDisk` — то же плюс `pool->forgetDisk(...)`»).
   Уточнение к обзору: `shutdown()` (`:921-942`) `lifecycle_mutex` НЕ берёт (это уже установлено
   {#cas-049}), но встаёт в ту же очередь через `gc_scheduler_mutex` (`:924`), который `forgetDisk`
   держит — так что следствие «shutdown queues behind it» верно, хотя механизм другой. Независимо от
   FORGET шатдаун диска платит тот же потолок напрямую: `~Pool` зовёт
   `mount_runtime.stopRemountThread()` первым шагом (`Pool/CasPool.cpp:879`).

7. **Самомаскирующий комментарий не только жив, но и размножился после rev.8.** rev.8 добавил шаг (1)
   `publishVanishedIntent()` перед джойнами и обосновал им бесплатность джойнов:
   - `Pool/CasPool.cpp:920-923` — «(1) Publish the terminal-intent latch FIRST (spec §5). The keeper
     callback stops arming remounts and the remount loop bails at its next step boundary, so **every
     join below is bounded to one step + one backend timeout.**»
   - `Pool/CasPool.cpp:944-946` — «A remount attempt already IN FLIGHT when step 1 published the intent
     completes its current step before the loop bails (**the "one step + one backend timeout" bound of
     §5**)…»
   Оба утверждения ложны ровно в одном месте: «current step» — это целиком `tryRemountOnce`, а он
   содержит `claimMountAwaitingExpiry`, чей потолок — 146 с, а не «один backend timeout». Терминальный
   интент действительно проверяется, но только на границах ВНЕ этой петли: `remountTerminal()` в
   условии `while` (`Pool/CasMountRuntime.cpp:461`) и `mount_runtime.isVanished()` в Step 0
   (`Pool/CasPool.cpp:1038-1039`) — то есть ДО входа в наблюдение. Если интент опубликован, когда
   попытка уже внутри петли наблюдения, джойн ждёт до конца всех четырёх окон.

8. Условие достижимости — то же, что назвал обзор: `claimMount` должен возвращать `LiveDoubleStart`
   (`Pool/CasServerRoot.cpp:519-520`), то есть слот выглядит занятым живым держателем. Это ровно
   картина остановленного из-за транзиентного сбоя продления: наш keeper перестал продлевать, слот
   ещё не истёк или его токен ещё меняется. При стабильном токене выход происходит по достижении
   `threshold_ms` (одно окно, 36.5 с); полные 146 с набегают, когда токен продолжает меняться и
   исчерпываются все 3 рестарта.

**Фиксящего коммита нет.** `git log 056488b47a0..HEAD -S "claimMountAwaitingExpiry"`,
`-S "remount_stop"`, `-S "kMaxObservationRestarts"` по поддереву `ContentAddressed/` дают только
коммиты СТАРШЕ обзора (`3e1b6358104`, `d8b401ff035`, `76694149adc`, `b4a24d56cf5`, `ba0a5231a3b`,
`d3dfb0f5d18`); `-S "should_stop"` — пусто. rev.8 работал в этой области, но добавил защёлку интента
и обоснование джойнов, а не прерываемость самого ожидания.

**Что перепроверка добавила сверх обзора.** Существующие адъюдикации соседей класс не закрывают:
{#cas-015} прямо цитирует `claimMountAwaitingExpiry` и удержание `remount_mutex`, но заключает «эти
ожидания ограничены (TTL-опрос + отменяемый join …)» — потолок там не считался, и teardown-джойн
(`stopRemountThread` из FORGET/`~Pool`) как отдельный пострадавший не рассматривался; её остаток (a)
про неотменяемость сформулирован через `KILL QUERY`/`max_execution_time`, то есть про запросы, а не
про lifecycle-верб. {#remount-running-latched-before-spawn} касается той же нити, но его вывод
(«costs at most one backoff interval (the wait is a `wait_for`)») относится именно к прерываемой
половине и по контрасту усиливает вывод здесь.

**Почему P2, а не P1.** Ожидание конечно (146 с — жёсткий потолок, не «навсегда»), порчи и потери
данных нет, отказ не громкий но и не тихий (`LOG_INFO` на входе в наблюдение,
`Pool/CasPool.cpp:1118-1125`; `Pool/CasServerRoot.cpp:561-565`), а пути данных `lifecycle_mutex` не
берут — блокируются только операторские верби и шатдаун (это уже установлено {#cas-049}: «Данные-пути
(`poolAccess`, чтение/запись, `gcHealth`) `lifecycle_mutex` не берут … Блокируются только операторские
verbs, не запросы»). Но выше P3 это поднимают два обстоятельства: (а) наиболее болезненный
пострадавший — шатдаун, где 146 с на CA-диск легко перерастает grace-период оркестратора (у
Kubernetes `terminationGracePeriodSeconds` по умолчанию 30 с) → SIGKILL → незаслуженное отсутствие
прощального маркера → преемник идёт медленным путём реклейма по наблюдению; (б) комментарий кода
активно утверждает обратное в двух местах, то есть дефект самомаскирующийся — следующий читатель
будет исходить из ложной границы.

**Что осталось / как закрывать.** Ровно предложение обзора, и оно дешёвое, потому что точка инъекции
уже есть: добавить `const std::function<bool()> & should_stop = {}` в `claimMountAwaitingExpiry`
(`Pool/CasServerRoot.h:421-429`) и проверять его в начале каждой итерации `while (true)` и перед
каждым из двух `sleep_ms_fn(poll)` (`Pool/CasServerRoot.cpp:544`, `:566`), возвращая отдельный вид
результата «прервано» (чтобы вызывающий не спутал это с `LiveDoubleStart`/`ForeignOwner`). На стороне
`tryRemountOnce` передать `[this] { return mount_runtime.remountStopRequested() ||
mount_runtime.remountTerminal(); }`. Дополнительно — заменить голый `sleep_for` на `:1115` на
прерываемое ожидание (тот же `remount_cv.wait_for` с предикатом, что уже используется для backoff'а на
`Pool/CasMountRuntime.cpp:466-467`), иначе прерывание будет иметь гранулярность `poll` = 5 с вместо
мгновенной. И исправить оба комментария (`Pool/CasPool.cpp:920-923`, `:944-946`), заменив «one step +
one backend timeout» на честную границу. Пункт нужно ЗАВЕСТИ в
`BACKLOG/mounts-and-lifecycle.md` — сейчас он не отслеживается ни там, ни в
`final-checks-todo.md`.

## B6 (дубликат CAS-047, P3) {#b6}

**Форма кода верна и на HEAD (один процессный пул, 16 потоков, `queue_size == max_threads`, блокирующий enqueue), но это штатный backpressure, уже адъюдицированный как by-design в CAS-047; остаток — отсутствие проверки отмены в ожидании (P3).**

**Заявлено (opus B6).** `initializeBlobUploadPool` использует 4-аргументный конструктор `ThreadPool`, из-за чего `queue_size == max_threads == 16`; `fanOutBlobUploads` планирует через `enqueueAndGiveOwnership` без `wait_microseconds`, попадая в `scheduleOrThrowOnError` → `job_finished.wait(lock, pred)` без таймаута. Следствия по opus: (а) неограниченное непрерываемое ожидание отправителя, (б) cross-tenant head-of-line blocking — 16 INSERT-потоков на тормозящем CA-диске `d1` останавливают запись на здоровый CA-диск `d2` в том же процессе, (в) одна широкая часть способна заполнить очередь сама, (г) отмена запроса не наблюдается ни в ожидании, ни в цикле join.

**Состояние на HEAD (`6ebc7ff6d2c`).** Форма кода не изменилась с момента триажа CAS-047:
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.cpp:31-33` — статические `pool_mutex` / `pool_instance` (пул один на процесс); `:36-50` `initializeBlobUploadPool` строит `std::make_unique<ThreadPool>(metric, metric_active, metric_scheduled, size)` — тот самый 4-аргументный конструктор, дающий `max_free_threads_ = queue_size_ = max_threads_` (`src/Common/ThreadPool.cpp:187-195`, затем `queue_size = max(queue_size_, max_threads)` на `:206`).
- `src/Core/ServerSettings.cpp:152` — `cas_blob_upload_pool_size` = 16. Итог: очередь = 16 = число потоков. Утверждение точное.
- `ContentAddressedTransaction.cpp:309` — единственный продовый вызов `Cas::fanOutBlobUploads(*st.build, requests, Cas::blobUploadPool())`; `:1767` — `runner.enqueueAndGiveOwnership(...)` без `wait_microseconds`; проверки отмены в цикле планирования/join нет (grep по `isCancelled` в файле — пусто).
- `:1696-1715` — группировка по уникальному `BlobRef` (`std::map<BlobRef, BlobUploadRequest> grouped`), т.е. задач ≤ числу уникальных блобов части.

**Дубликат.** CAS-047 (`docs/superpowers/cas/2031-triage.md:2187`) разбирает ровно этот механизм и закрывает его как **by-design, P3**: пул умышленно процессный и отделён от `IObjectStorage::getThreadPoolWriter`, чтобы вложенная отправка S3-multipart не давала wait-on-self дедлок (`Pool/CasBlobUploadPool.h:13-25`); отправляющий поток слота не занимает, а задачи (`PartWriteTxn::uploadBlobDetached`) на этот же пул ничего не планируют — поэтому ожидание конечно, а не «unbounded»: сверху оно ограничено дедлайном операции `CasRequestController` (90 с на попытку). «Все загрузки сериализуются» неверно — 16 задач идут параллельно, полная очередь = классический backpressure. Размер пула — серверная настройка, и `docs/en/antalya/cas/operations/troubleshooting.md:20` уже рекомендует её ПОНИЖАТЬ при устойчивых `SlowDown`, т.е. рычаг оператора в обе стороны существует.

Fable M6 (`docs/superpowers/cas/fable-review-triage.md:289`) — про тот же файл, но про другое: сырая `ThreadPool &` уходит из-под отпущенного `pool_mutex` и `clickhouse-local` гасит пул ДО `global_context->shutdown()`. К B6 не относится.

**Чем формулировка opus отличается от сиблинга.** CAS-047 сфокусирован на «сериализации/дедлока нет, это backpressure» и на числовой арифметике находки. Opus добавляет одну содержательную ось, которой у сиблинга нет явно: **межтенантная несправедливость** — пул общий для ВСЕХ content-addressed дисков процесса, поэтому один тормозящий бакет расходует все 16 слотов и притормаживает запись на здоровые CA-диски. Это реальное свойство, но по величине оно ограничено тем же 90-секундным дедлайном задачи и лечится той же настройкой размера пула, т.е. приоритет не меняет: единого пула ради anti-wait-on-self инварианта хотели специально, а изоляция per-disk была бы новым дизайном, не багфиксом.

**Что осталось.** Ровно остаток CAS-047: ожидание в `ThreadPool::scheduleImpl` не проверяет отмену запроса, поэтому INSERT/merge, упёршийся в полную очередь, не реагирует на `KILL QUERY` до освобождения слота (≤ дедлайн операции). Это свойство общего апстримного `ThreadPool`, не CAS-кода; лечится либо `queue_size > max_threads`, либо дедлайном в `scheduleOrThrow` — и то и другое входит в уже затреканный замер `{#writepath-candidates-post-stage1}` (2). Новая запись в BACKLOG не нужна.

## B7 (дубликат CAS-036, P3) {#b7}

**Механика на HEAD жива — тело control-объекта читается целиком до срабатывания `object_cap`, хотя авторитетный размер уже известен из HEAD; но по урегулированной модели доверия триггер — держатель bucket-креденшла, отказ громкий, тихой порчи нет, поэтому P3 (адъюдицировано в CAS-036).**

**Заявлено (opus B7).** `Backend::head()` отдаёт `HeadResult::size` до `get()`, но ни один вызывающий не сравнивает этот размер с зарегистрированным `object_cap` формата перед `get()`. `get()` → `readObjectRanged()` при `range.whole()` делает неограниченный `readStringUntilEOF`; cap проверяется только в `openObject` уже по материализованным байтам. Паттерн повторяется на всех call-site'ах `openObject` (`CasGc.cpp`, `CasOrphanManifestSweep.cpp`, `CasPartWriteTxn.cpp`, `CasPool.cpp`, `CasRefLedger.cpp`, `CasRefProtocol.cpp`, `CasFsck.cpp`, `CasInspect.cpp`). Следствие по opus: любой, кто может записать или испортить один control-объект (манифест, ref-log, ref-snapshot, ref-catalog, ref-ckpt, gc-state, pool-meta, owner/epoch/mount-lease), заставляет каждый читающий сервер материализовать полный размер объекта — дешёвый повторяемый OOM, обесценивающий `CasByteBudget.h`/`FormatTraits::object_cap`. Write-side гейты (`kMaxManifestEntries`, `kMaxManifestEncodedBytes`, `ref_txn_max_ops`) ограничивают только то, что сервер пишет сам, а не то, что он принимает при чтении. Явно и корректно оговорено, что zstd-путь защищён до декомпрессии, и что это линейно по байтам атакующего, а не амплификация.

**Состояние на HEAD (`6ebc7ff6d2c`) — механика подтверждается.**
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:380-390` — `readObjectRanged(..., uint64_t known_size = 0)`, при `range.whole()` просто `readStringUntilEOF(content, *buf); return content;` — без лимита.
- `:618-646` — `ObjectStorageBackend::get` в Native-режиме сначала `auto hr = nativeHead(key);` (`:623`), затем `gr.bytes = readObjectRanged(*object_storage, key, range, hr->size);` (`:646`). Авторитетный размер в руках, но сравнивается только с настройкой буфера, не с cap'ом.
- Cap впервые срабатывает после материализации: `Formats/CasTextFormat.cpp:389-391` (`stored.size() > t.object_cap` → `CORRUPTED_DATA`) и `:401-403` (объявленный размер zstd-фрейма — до `resize`, т.е. эта половина действительно защищена, как opus и пишет).
Так что «между HEAD и cap стоит `String` произвольного размера» — верно и сейчас.

**Дубликат.** CAS-036 (`docs/superpowers/cas/2031-triage.md:2106`, строка таблицы `:54`) разбирает ровно это и закрывает как **частично, P3**, с двумя поправками, применимыми и к формулировке opus:
1. «OOMs the victim» — завышение по классу: аллокация `String` идёт через переопределённый `operator new` и учитывается MemoryTracker'ом, поэтому исход — громкое `MEMORY_LIMIT_EXCEEDED` на GC/mount/recovery-потоке вместо запинённого `CORRUPTED_DATA`. Это дефект качества диагностики и устойчивости, не порча данных.
2. «Anyone able to write or corrupt one control-object key» — по урегулированной позиции положить такой объект может только владелец bucket-креденшла, а он и есть вся граница доверия пула (`BACKLOG/docs-and-cleanup.md` {#pool-trust-boundary-undocumented} / CAS-027): «the bucket credential IS the whole trust boundary». Та же сторона может просто удалить пул. Легальный писатель такой объект не породит — pre-PUT гейты (`Formats/CasFoldSealFormat.cpp:165-192`, `Formats/CasRefCatalogFormat.h:124`, `Formats/CasRefLogFormat.cpp:49-69`) отказывают до записи. Естественная порча (битрот, обрыв записи) размер объекта не увеличивает.
Предложенный opus фикс совпадает с зафиксированным в BACKLOG направлением: дать `get`/`getStream` необязательный ожидаемый cap (или тонкий `getControlObject(FormatId, key)`) и отказывать по `hr->size > object_cap` до чтения.

**Чем формулировка opus отличается от сиблинга.** CAS-036 бил по трём механикам сразу и две из них снял как фактически неверные («read buffer по объявленному размеру», «resize по объявленному размеру zstd-фрейма»); opus этих двух ошибок не делает — он изолирует ровно живую половину и, в отличие от сиблинга, добавляет две вещи: (а) полный список call-site'ов `openObject`, показывающий, что паттерн системный, а не локальный для одного читателя; (б) аргумент, что write-side admission-гейты (`kMaxManifestEntries`, `kMaxManifestEncodedBytes`, `ref_txn_max_ops`) намеренно ограничивают только исходящую запись и потому не являются защитой на чтении — это чёткая формулировка того, почему cap на чтении нужен как отдельный механизм. Зато opus ставит находку в security-лейн («defeating the stated purpose … cheap repeatable OOM»), что по урегулированной модели доверия неверно: приоритет остаётся P3.

**Что осталось.** Ровно остаток CAS-036, уже заведённый и закоммиченный: (а) гейт по `hr->size` до чтения тела, чтобы ошибка была запинённой `CORRUPTED_DATA`, а не `MEMORY_LIMIT_EXCEEDED`; (б) независимая вторая половина того же раздела — Θ(k²) дедупликация JSON-ключей линейным `std::find` по `std::vector<String>` (`Formats/CasTextFormat.cpp:173-175`) при 64-MiB line cap у `RefLog`/`RefSnapshot`. Оба — P3, не гейт релиза.

## B8 (подтверждено, P2) {#b8}

**Подтверждено на HEAD: `rebuildBaseline` корректно классифицирует недекодируемый `gc/state` как свою же катастрофу, проходит гейт, а затем безусловно падает `CORRUPTED_DATA` на повторном decode внутри `acquireOrRenewLease`, так что DR-команда неработоспособна ровно в том сценарии, для которого существует.**

**Заявлено (opus B8).** `rebuildBaseline` открывается охраняемым decode `gc/state`, чей комментарий прямо называет цель («an undecodable `gc/state` IS scenario (а), the disaster this command exists for»), после чего проваливается в `acquireOrRenewLease(state, state_token, allow_steal=false)`, где для присутствующего объекта делается `GcState current = decodeGcState(got->bytes);` БЕЗ try/catch. `decodeGcState` бросает `CORRUPTED_DATA` при отсутствии ключа `gcs`, `gc_shards == 0`, хвостовых байтах или битой строке заголовка. Ни `runContentAddressedGcRebuild`, ни `CommandCaGcRebuild::executeImpl` это не ловят. `GC DRY RUN` падает так же. Единственный обходной путь — удалить объект внешним S3-клиентом, чтобы сработал bootstrap по отсутствию, — не назван ни в одном сообщении об ошибке.

**Состояние на HEAD (`6ebc7ff6d2c`) — подтверждается полностью, механика прослежена end-to-end.**
- `Gc/CasGc.cpp:3868-3884` — GET вынесен ЗА try (`:3869`), внутри try только `decoded = decodeGcState(got->bytes);` с `catch (...) // NOLINT(bugprone-empty-catch)` и комментарием «undecodable state = scenario (а)». `healthy` остаётся `false`.
- `:3955-3990` — ветка `if (!decoded || decoded->snap_generation == 0)`: `newestFoldSealRef()` перечислением, при недекодируемом/исчезнувшем seal — `CORRUPTED_DATA` с внятной ремедиацией («this pool must be recreated»), иначе `prior_seal` подхватывается либо ставится `rep.virgin_by_enumeration`. Эта защитная половина работает как задумано.
- `:3995-4001` — гейт `if (healthy && !force && !validate_generation_zero_ref_baseline)` не срабатывает (`healthy == false`), т.е. команда идёт дальше — и это правильное поведение.
- `:4010` — `if (!acquireOrRenewLease(state, state_token, /*allow_steal=*/false))`.
- `:4507-4539` — внутри `acquireOrRenewLease` для attempt 0: `const auto got = store->backend().get(key);` (`:4513`), объект ПРИСУТСТВУЕТ (это те же самые недекодируемые байты), ветка `if (!got)` с bootstrap-CAS не берётся, и на `:4539` выполняется `GcState current = decodeGcState(got->bytes);` — вне какого-либо try. Исключение уходит наружу.
- Ловца нет: `src/Interpreters/InterpreterSystemQuery.cpp:2585` — `Cas::RebuildReport rep = ca->runGcRebuildNow(force);` без try; ловится только `rep.refusal` (`:2587`), т.е. структурированные отказы, а не исключения.
- `GC DRY RUN` падает симметрично: `Gc::previewDeletes` (`Gc/CasGc.cpp:4407`) делает `const GcState state = decodeGcState(state_bytes->bytes);` на `:4414` вне try.
Итог: при present-and-undecodable `gc/state` каждый плановый раунд падает, `SYSTEM CAS GC REBUILD <disk> FORCE` тоже падает, `GC DRY RUN` тоже — GC пула стоит, а названная команда восстановления недоступна. Обходной путь (удалить объект внешним клиентом, чтобы `!got`-ветка на `:4516-4536` сделала bootstrap-CAS) в тексте ошибки не упоминается.

**Покрытие тестами — отсутствует ровно на этой ветке.** В `src/Disks/tests/gtest_cas_gc_rebuild.cpp` все rebuild-тесты моделируют ПОТЕРЮ состояния, а не его порчу: `:120`, `:412`, `:492` — `deleteExact(layout.gcStateKey(), token)`. Единственный тест со словом «Damaged» — `DamagedGenerationZeroStatePerformsNoCatalogDrainMutation` (`:356`) — про generation-0 state, а не про недекодируемые байты. Ни один тест не кладёт мусор в `gcStateKey()`, т.е. предложенный opus тест действительно отсутствует.

**Отношение к сиблингам (частичное перекрытие, но НЕ дубликат).**
- CAS-069 (`docs/superpowers/cas/2031-triage.md:87`, BACKLOG `gc.md:188` {#rebuild-gcstate-decode-reason-unreported}) разбирает ТОТ ЖЕ пустой catch, но только его диагностическую половину: потеряна причина, почему `gc/state` не декодировался. CAS-069 явным образом объявляет защитную половину («перечисление seal + отказ») неизменяемой и на этом закрывает вопрос — до `acquireOrRenewLease` он не доходит и о том, что команда всё равно упадёт дальше, не говорит.
- CAS-095 (`:113`, BACKLOG `gc.md:362` {#gc-dryrun-silent-on-damaged-state}) касается `previewDeletes`, но приходит к обратной оценке той же строки `:4414`: там громкий `CORRUPTED_DATA` подан как ПРАВИЛЬНОЕ поведение в контраст к тихому `preview_deletes=0` при отсутствующем состоянии. С точки зрения B8 громкость на dry-run — не дефект; дефект в том, что при этом нет ни одного входа, который бы состояние починил.
- CAS-094 ({#rebuild-refusal-leaves-run-and-seal-residue}) — про residue после отказа по проигранному CAS, другой путь.
- Класс в целом уже описан: `BACKLOG.md` {#damaged-object-repair} (`:112-130`) прямо формулирует «the system fails closed forever and hands the operator no lever» и включает `gc/state` в список объектов, которым нужны и различение present-and-undecodable/absent/decodable-but-inconsistent (пункт 1), и repair-верб (пункт 2). B8 — конкретный, самый острый экземпляр этого класса.

**Чем формулировка opus отличается от сиблингов.** Сиблинги трактуют пустой catch как потерю диагностики, а громкий throw — как корректный fail-closed. Opus единственный проследил ПОСЛЕДОВАТЕЛЬНОСТЬ: классификация верна → гейт пройден → и тут же безусловный повторный decode тех же байтов в захвате lease, т.е. правильная классификация ничего не даёт, потому что после неё нет ни одной ветки, способной перезаписать испорченное состояние. Это превращает диагностический P3 сиблингов в операбельный P2: named disaster-recovery команда не выполняет свою заявленную функцию, и оператор об этом узнаёт из `CORRUPTED_DATA` без ремедиации. Плюс opus даёт конкретную и правильную форму фикса: классифицировать ОДИН раз и передать вердикт в захват lease — bootstrap-режим, CAS-ящий свежий `GcState` против НАБЛЮДЁННОГО токена испорченного объекта (никогда безусловно), плюс тест с мусором в `gcStateKey()`.

**Что осталось.** Всё. Минимум — поймать исключение в `acquireOrRenewLease`-пути rebuild-а и перебросить с точной ремедиацией (как это уже сделано для seal на `:3916-3921`). Правильно — bootstrap против наблюдённого токена + тест. Триггер — байтовое повреждение durable-объекта, что вне принятой модели отказов доверенного стора, и потери данных нет (fail-closed, обходной путь существует), поэтому P2, а не P1: гейтом релиза не является.

## B9 (дубликат fable-B4, P3) {#b9}

**Тест по-прежнему отключён тегом, concurrent-покрытия под CA нет, атомарной публикации pointer-объекта нет — но «B66a не отслеживается нигде» опровергнуто (артефакт ветки поставки без `docs/superpowers/`), сам механизм из комментария теста устарел после B181, а класс — только local/emulated бэкенд.**

**Заявлено (opus B9).** Тег `no-cas-storage` на `03350_alter_table_fetch_partition_thread_pool.sql:2` своим же обоснованием признаёт дефект: параллельные fetch'и 100-партовой партиции read-modify-write'ят ОБЩИЙ «detached» ref-объект, чтение этого горячего pointer-объекта не сериализовано против усекающей in-place перезаписи локального object storage, отсюда torn ref/manifest (`CANNOT_READ_ALL_DATA` / `NO_FILE_IN_DATA_PART`); атомарная публикация pointer-объекта — отложенный пункт (B66a). Ничто покрытие не заменяет: `05002_cas_fetch_partition.sql` делает ровно один непараллельный FETCH, `04289_cas_multi_detach_drop.sql` покрывает другой, уже решённый write-side баг. `ALTER TABLE … FETCH PARTITION` с thread pool — рядовая административная операция, а не экзотика; ограничение отсутствует и в списке known limitations `roadmap.md`. Область, судя по механизму (усекающая in-place перезапись), ограничена `LocalObjectStorage`, но названный фикс («atomic pointer-object publish») намекает на общий дизайнерский пробел, а quick-start рекомендует именно локальный конфиг.

**Состояние на HEAD (`6ebc7ff6d2c`).**
- Тег на месте, текст обоснования дословно тот же: `tests/queries/0_stateless/03350_alter_table_fetch_partition_thread_pool.sql:2` — `-- no-cas-storage: … a concurrent reader can see a torn ref/manifest … deferred backlog item (B66a); single-part FETCH works (01650 + 05002).` Единственный коммит, менявший файл после базы ревью, — `c4f0ba4184f` (механическое переименование тега `no-content-addressed-storage` → `no-cas-storage`).
- Concurrent-покрытия под CA нет: в `tests/queries/0_stateless/05002_cas_fetch_partition.sql` ноль вхождений `concurrent`/`parallel`/`thread_pool`.
- Атомарной публикации нет: `Backend/CasObjectStorageBackend.cpp:571` — `emuWrite` по-прежнему `object_storage->writeObject(StoredObject(emuPath(key)), WriteMode::Rewrite, attrs)`, а `LocalObjectStorage::writeObject` открывает `WriteBufferFromFile` прямо на финальном ключе (ни temp-файла, ни `rename`).
- Отсутствие в документации подтверждается: `docs/en/antalya/cas/roadmap.md:71-88` — в списке known limitations четыре пункта (Azure, прочие S3-совместимые, `encrypted` над CAS, нестабильность формата), про concurrent FETCH на локальном бэкенде ничего.

**Дубликат.** fable-B4 (`docs/superpowers/cas/fable-review-triage.md:244`, строка таблицы `:22`) — та же находка, вердикт **частично, P3**, и она снимает две части формулировки:
1. «B66a is tracked nowhere» — не подтвердилось и является методологическим артефактом: `git ls-tree 056488b47a0 docs/superpowers` пусто, т.е. на ветке поставки, по которой шло ревью, внутреннего каталога просто нет. На HEAD `docs/superpowers/cas/BACKLOG/formats-and-storage.md:55` содержит именной пункт **[B66a]** («concurrent-fetch torn read of a shared `detached` ref on local storage … Safe on S3 (atomic PUT) … racy multi-writer on local/NFS stays documented-unsafe»), заведённый ещё `45a6c8ee2b6` (2026-07-13), т.е. ДО ревью.
2. Механизм в комментарии теста устарел: общего «detached» ref-объекта больше нет — detached-части живут как ОТДЕЛЬНЫЕ ref'ы `detached/<part>` внутри namespace своей таблицы (`Parts/PartPathParser.h:43-49`, `kDetachedRefPrefix`, «One namespace per table… No parallel detached namespace exists anymore»), это работа B181 (`a0493e26d0f`, `0e40a1f0fc3`, `d74c12d9576`, `fa4c7aad733`), тоже до головы ревью. Fan-out на 100 РАЗНЫХ частей пишет 100 разных ref-ключей; остаточный случай — два конкурентных писателя ОДНОГО имени `detached/<part>`, чего именно этот тест не создаёт. Поэтому первым действием сиблинг ставит прогон `03350` под CA-полосой: если премисса устарела вместе с B181, тег снимается бесплатно и дыра в CI закрывается без строчки продуктового кода. Это действие на HEAD НЕ выполнено.
Кроме того класс — только `Emulated`/local; на Native/S3 публикация ref — атомарный PUT, а local/NFS раздел бэклога сам себя объявляет `documented-unsafe` для multi-writer («Nothing in this section affects S3/GCS production pools»).

**Чем формулировка opus отличается от сиблинга.** Два добавления, оба уместные. Во-первых, opus точно называет, ПОЧЕМУ замены покрытия нет, разобрав оба кандидата: `05002` — один непараллельный FETCH, `04289_cas_multi_detach_drop.sql` — другой, уже закрытый write-side баг (последовательные DETACH-коммиты, перезаписывавшие общий ref); сиблинг проверял только `05002`. Во-вторых, opus формулирует запасной вариант фикса, которого у сиблинга нет: если продуктовый фикс отложен на релиз, добавить тест уменьшенного объёма, пиннящий ЗАДОКУМЕНТИРОВАННОЕ fail-closed поведение, чтобы будущий переход к ТИХОЙ порче был пойман — это правильный ход, потому что дешёвый и он покрывает именно тот риск, который отложенность создаёт. С другой стороны, opus подсвечивает quick-start с локальным конфигом как усиливающий фактор, а это как раз то, что сиблинг парирует ссылкой на `documented-unsafe`-статус локального бэкенда.

**Что осталось (в порядке дешевизны).** (а) прогнать `03350` под CA-полосой и проверить, заслужен ли ещё тег после B181; (б) если падает — привести комментарий теста в соответствие с реальным механизмом (ref-лог namespace'а на неатомарном локальном бэкенде, а не «shared detached ref»); (в) продуктовый фикс `[disk-error-audit]` (temp-file + `rename`), который бэклог прямо называет закрывающим механизм B66a; (г) добавленное opus: либо reduced-scope тест на fail-closed, либо строка в `roadmap.md` known limitations. P3 и не pre-release: на S3/GCS-пулах — единственной production-посадке — класс не воспроизводится; реальная цена сегодня — слепое пятно в CI и, возможно, лишний тег на тесте, который бы уже проходил.

## M1 (подтверждено, P3) {#m1}

**Третий дизъюнкт — поиск подстроки `PreconditionFailed` в сообщении сервера — на HEAD жив и по-прежнему консультируется в глобальном `Client::RetryStrategy::ShouldRetry` для всего S3-трафика; комментарий переписан лишь частично.**

**Что заявлено.** `S3::isPreconditionFailedError` содержит третий дизъюнкт
`error.GetMessage().find("PreconditionFailed") != npos` — сканирование подстроки в сообщении,
которое приходит от сервера. Предикат безусловно вызывается из `Client::RetryStrategy::ShouldRetry`,
т.е. на пути ВСЕГО S3-трафика (бэкапы, Iceberg, `s3queue`, обычные S3-диски). Для CAS-вызывающего
over-match безопасен (форсит re-validate), а для `ShouldRetry` направление инвертировано: over-match
подавляет ретрай реально транзиентной ошибки. Плюс: комментарий с внутренним тикет-ID, и нет теста
на условный commit Iceberg.

**Что на HEAD.** Форма кода воспроизводится дословно.

`src/IO/S3Common.h:94-99`:
```cpp
inline bool isPreconditionFailedError(const Aws::Client::AWSError<ErrorType> & error)
{
    return error.GetResponseCode() == Aws::Http::HttpResponseCode::PRECONDITION_FAILED
        || error.GetExceptionName() == "PreconditionFailed"
        || error.GetMessage().find("PreconditionFailed") != std::string::npos;
}
```

`src/IO/S3/Client.cpp:108-114` — консультация безусловна и стоит ДО проверки `attemptedRetries`,
`isQueryCanceled` и `error.ShouldRetry()`, т.е. это первый по порядку глобальный обрыв ретрая
(после `MOVED_PERMANENTLY`):
```cpp
    /// ... downstream SYSTEM SYNC REPLICA (B166). One 412 policy across the retry and CA conditional ops.
    if (S3::isPreconditionFailedError(error))
        return false;
```

Правки после ревью были, но косметические: `354b1df855f` («final-review polish — truthful S3
predicate comment») переписал комментарий над предикатом в `S3Common.h:70-93` так, что он теперь
честно объявляет message-fallback и объясняет мотивацию (не-AWS тело, RustFS, пустой `ExceptionName`),
и убрал оттуда внутренний тег. Сам дизъюнкт не тронут. Внутренний тег `B166` при этом остался в
`src/IO/S3/Client.cpp:111` и в имени/комментарии теста
`src/IO/S3/tests/gtest_aws_s3_client.cpp:215-219` — это уже территория M8, но половина «rewrite the
comment without the internal ticket ID» выполнена только в одном из трёх мест.

Существующий тест `IOTestAwsS3Client.DoesNotRetryPreconditionFailed`
(`src/IO/S3/tests/gtest_aws_s3_client.cpp:214-241`) закрепляет как раз ЖЕЛАЕМОЕ поведение обеих
безопасных ветвей (response code и exact `ExceptionName`) и отдельно проверяет, что
`SLOW_DOWN`/`SERVICE_UNAVAILABLE` ретраится. То есть предложенный фикс (снять message-дизъюнкт с
пути `ShouldRetry`) существующие тесты НЕ ломает: ни один тест не требует message-матчинга именно на
retry-пути (строка `:237` проверяет `named`, где совпадает `ExceptionName`).

Теста на условный commit Iceberg (`writeMetadataFileAndVersionHint`,
`src/Storages/ObjectStorage/DataLakes/Iceberg/Utils.cpp`) по-прежнему нет.

**Дубликат?** Нет. `2031-triage.md` CAS-021 (`:875`) и CAS-010 (`:428`) — про контроллер условной
записи и пустой токен, они трогают отображение `NoSuchKey → PreconditionFailed` в
`Backend/CasObjectStorageBackend.cpp:156-159`, но не глобальный retry-путь. `fable-review-triage.md`
V6 (`:74`, `:689-691`) разбирает `copyObjectConditional` и тоже не касается `ShouldRetry`.

**Что осталось / приоритет.** Механика подтверждена, но достижимость узкая: response-code-проверка
стоит первой, поэтому вред возникает только для ошибки, которая НЕ является 412, но чьё тело
содержит подстроку `PreconditionFailed` — например 5xx от S3-совместимого шлюза, эхом
пересылающего исходный текст. Демонстрируемого пути на HEAD нет, а последствие — потеря ретрая
(громкая ошибка запроса), не порча данных. Отсюда P3, не до релиза. Остаток: (а) снять
message-дизъюнкт с пути `ShouldRetry`, оставив его только CA-вызывающим (например расщепить на
`isPreconditionFailedError` и `isPreconditionFailedErrorLenient`); (б) добить `B166` в `Client.cpp:111`
и в тесте; (в) тест на условный commit Iceberg.

## M2 (дубликат CAS-066, P2) {#m2}

**Дубликат CAS-066 (адъюдицировано by-design): выбор режима по типу хранилища и INFO-уровень подтверждены и на HEAD, единственный живой остаток — отсутствие оговорки «только один сервер» в `quick-start.md`/`configuration.md`.**

**Что заявлено.** `local` object storage молча включает `EmulatedSingleProcess`; комментарий сам
признаёт, что два сервера над одним локальным пулом «would silently violate the CAS invariants — the
capability probe cannot detect this», а уровень лога понижен до INFO именно чтобы не валить ~15
stateless-тестов; при этом ограничение «один сервер» не упомянуто нигде в `docs/en/antalya/cas/**`,
а `quick-start.md` рекомендует ровно эту конфигурацию как самый простой вход.

**Что на HEAD.** Всё воспроизводится дословно, файл переехал не был (CAS-код в подкаталоги двигал
`592b9b83568`, но сам `ContentAddressedMetadataStorage.cpp` остался на месте), сместились только
строки.

`ContentAddressedMetadataStorage.cpp:689-693` — выбор режима по типу хранилища, без ручки:
```cpp
    const auto mode = object_storage->getType() == ObjectStorageType::Local
        ? Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess
        : Cas::ObjectStorageBackend::Mode::Native;
```
`:697-710` — тот же комментарий («…would silently violate the CAS invariants — the capability probe
cannot detect this (each process passes it alone). Make a shared-pool misconfiguration visible at
INFO, not WARNING») с тем же обоснованием про `send_logs_level=warning` и с той же отложенной
альтернативой «a future `system.warnings` entry could restore a louder, test-safe signal».
`:711-717` — сам `LOG_INFO` с текстом «safe ONLY for a single server… Do NOT share this pool path
between multiple ClickHouse servers (e.g. a shared/NFS mount)».

Механизм `system.warnings` НЕ подключён: единственное упоминание `system.warnings` во всём
CAS-каталоге — это тот самый комментарий (`grep -rn "system.warnings\|addWarningMessage"` по
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` даёт одно попадание, `:708`).

Документационная половина тоже подтверждена на HEAD: `grep -rn "single server\|single-server\|NFS\|
shared mount\|multi-server" docs/en/antalya/cas/` — **ноль** попаданий. При этом
`docs/en/antalya/cas/quick-start.md:15` по-прежнему подаёт local как беспроблемный онрамп («This
example uses the `local` object-storage backend so it needs…») и `:24` показывает
`<object_storage_type>local</object_storage_type>`. В `configuration.md` `local` фигурирует только
как значение `staging_backend` (`:104`), основной пример — `s3` (`:26`), т.е. страницы конфигурации
эта оговорка тоже не касается.

**Почему дубликат.** `2031-triage.md` CAS-066 (`:2947-2985`) разбирает ровно этот код и вынес
**by-design, P2**: режим — свойство возможностей хранилища, а не вкуса оператора; обе «неправильные»
комбинации закрыты (Native над не-S3 отклоняется громко в `Backend/CasObjectStorageBackend.cpp:94-110`
через `supportsRetryProfile(SingleAttempt)`; Emulated над S3 недостижим по конструкции); INFO —
зафиксированное в коде решение с обоснованием; единственный остаток — user-facing оговорка,
отнесённая к уже существующему B26/B135 внутри `{#local-backend}`.

**Что опус-формулировка добавляет к сиблингу.** CAS-066 закрыл вопрос как чисто документационный
долг («без кода-фикса»); опус, наоборот, называет вторую, кодовую половину фикса — восстановить
*громкий* сигнал через `system.warnings`, т.е. предлагает исполнить ту самую отложенную
альтернативу, которую комментарий кода сам себе выписал, вместо того чтобы оставить сигнал на INFO
навсегда. Плюс опус явно расширяет doc-фикс на `configuration.md` (CAS-066 указывал только
`quick-start.md`) и подчёркивает противоречие: страница, рекомендующая конфигурацию, молчит о том,
что сам код называет «would break silently».

**Что осталось.** (а) оговорка «single server only, не разделяемый/NFS путь» в `quick-start.md`
рядом с примером на `:24` и в `configuration.md`; (б) опционально — запись в `system.warnings`
вместо/в дополнение к INFO (test-safe, не форвардится клиенту). Приоритет остаётся P2 как у
CAS-066: цена ошибки высокая, вероятность низкая, релиз не блокирует.

## M3 (подтверждено, P2) {#m3}

**Обе половины подтверждены дословно: ни один из семи терминальных счётчиков не упоминается в `docs/`, а `CASIdentityLost`/`CASDataRootVanished` — состояния, которые код сам называет TERMINAL — по-прежнему логируются на `LOG_WARNING`, тогда как алерт по ERROR настроен на `CASMountExclusivityViolation`.**

**Что заявлено.** Семь из девяти приоритетных для алертинга событий
(`CASMountExclusivityViolation`, `CASIdentityLost`, `CASDataRootVanished`, `CASMountLeaseLost`,
`CASGCStuckRemovals`, `CASConditionalWriteFenceLostPostWrite`, `CASRemountHeldTransient`) не
встречаются нигде в дереве документации, при том что их собственные описания в `ProfileEvents.cpp` —
самые тяжёлые в наборе; курированная таблица `monitoring.md` вместо них перечисляет счётчики
contention/dedup. Сверху — инверсия уровней: `CASMountExclusivityViolation` пишется на `LOG_ERROR`, а
два состояния, которые код сам называет TERMINAL, — на `LOG_WARNING`, так что алерт «только ERROR»
пропускает именно терминальные случаи.

**Что на HEAD, половина 1 (документация).** Подтверждено ровно как заявлено.
`grep -rn` по всем семи именам в `docs/` даёт: сам обзор
(`docs/superpowers/cas/random/opus-review-20250805.md:411-421`), плюс `CASConditionalWriteFenceLostPostWrite`
в `fable-review-triage.md:486` и `2031-triage.md:974,4172`, плюс `CASRemountHeldTransient` в
`2031-triage.md:2739`. В `docs/en/**` — **ноль** попаданий по всем семи.

Курированная таблица `docs/en/antalya/cas/operations/monitoring.md` (`## Key metrics {#key-metrics}`,
`:29`) содержит десять строк (`:38-47`) и все они — contention/dedup/GC-progress:
`CASBlobCompareSwapConflict`, `CASBlobHeadFirst`/`CASBlobBodyPutAvoided`, `CASRefAppendWedged`,
`CASRefNeedsRecovery`, `CASRefAppendSealRejected`, `CASGCHeartbeatFenceOuts`,
`CASGCUnmatchedRemoveDeltas`, `CASGCCondemnMarkerUnconfirmedCarry`, `CASGCMetaWriteAnomaly`,
`CASRefRollbackBestEffortDropFailed`. Ни одного из семи.

Сами счётчики живы и описания действительно самые тяжёлые в наборе —
`src/Common/ProfileEvents.cpp:915` (`CASGCStuckRemovals`), `:929`
(`CASConditionalWriteFenceLostPostWrite`), `:930` (`CASMountLeaseLost`), `:932`
(`CASMountExclusivityViolation`: «This is the single-writer guarantee being broken… **This must always
be zero**»), `:933` (`CASRemountHeldTransient`), `:934` (`CASIdentityLost`: «IdentityLost is a
**fail-loud TERMINAL state**»), `:935` (`CASDataRootVanished`: «entered a **terminal Vanished**
lifecycle state»).

**Что на HEAD, половина 2 (инверсия уровней).** Тоже подтверждено дословно.
- `Pool/CasMountRuntime.cpp:363-364`: `ProfileEvents::increment(ProfileEvents::CASIdentityLost);`
  сразу за ним `LOG_WARNING(getLogger("CasPool"), "…entered IdentityLost: … This is a fail-loud
  TERMINAL state: store-class access now fails loud and this pool's remount + GC threads self-exit…")`
  — то есть текст сам говорит TERMINAL, а уровень WARNING.
- `Pool/CasMountRuntime.cpp:415-419`: `ProfileEvents::increment(ProfileEvents::CASDataRootVanished);`
  + `LOG_WARNING(… "entered Vanished({}) … store-class access now fails loud with a typed error")`.
- `Pool/CasServerRoot.cpp:1242-1250`: `ProfileEvents::increment(ProfileEvents::CASMountExclusivityViolation);`
  + `emitMountEvent(..., CasEventType::MountConflict, ...)` + `LOG_ERROR(getLogger("CasMountLeaseKeeper"), …)`.
  Комментарий над этим местом (`:1232-1241`) объясняет, почему тут нет `chassert`/abort, но про
  соотношение с уровнями двух терминальных состояний ничего не говорит.

Т.е. на HEAD ровно три уровня для трёх событий одного класса «инвариант сломан / состояние
терминально»: ERROR у одного и WARNING у двух более тяжёлых.

**Дубликат?** Нет. Ближайший сиблинг `fable-review-triage.md` M13 (`:478`) — про отсутствие
`ProfileEvent` у `throwCasWriteRetryLater`, про `gc_scheduler_running` и про тестовые пробелы; он не
касается ни публикации терминальных счётчиков в `monitoring.md`, ни уровней логов.
`BACKLOG/operability-and-introspection.md` `{#profileevents-surface-residuals}` (`:366`) разбирает
другие два дефекта той же поверхности (мёртвая строка `CASServer*`, перекос
`CASBlobBodyPutAvoided`) и прямо отмечает, что «Mount and lease activity itself is not
unobservable», т.е. подтверждает наличие механизма — но не его документированность.

**Что осталось.** (а) добавить семь счётчиков в таблицу `monitoring.md` с пометкой «healthy = zero» и
указанием, что делать (для `CASMountExclusivityViolation` — «this must always be zero», прямая цитата
из описания события); (б) поднять `CASIdentityLost` и `CASDataRootVanished` до `LOG_ERROR`
(`Pool/CasMountRuntime.cpp:364`, `:416`), чтобы ERROR-алерт покрывал терминальные состояния;
(в) при этом стоит заодно свести таблицу и `system.cas_mounts.lifecycle` в один раздел, чтобы
оператор видел, как счётчик соотносится с наблюдаемым состоянием пула. P2: данные не под угрозой,
счётчики существуют и запрашиваемы через `system.events`, дефект — в том, что оператор не знает, за
чем следить; но это релизный doc-долг того же класса, что **[B197]/[B198]** (помеченные GATE), так
что закрывать до объявления фичи операционно поддерживаемой.

## M4 (дубликат CAS-098, P2) {#m4}

**Дубликат CAS-098 пункт 1; ключевая проверяемая часть — «фикс уже есть, но не используется» — ПОДТВЕРЖДЕНА дословно: `ever_succeeded` вычисляется в `gcHealth` и не потребляется ни одним продакшн-читателем (только двумя gtest'ами), а SQL-колонка уже `Nullable(UInt64)` и уже вставляет NULL на peer-строках.**

**Что заявлено.** Лидер GC (`is_leader = 1`), у которого каждый раунд падал до `Finish`, вечно
отдаёт `last_success_age_seconds = 0` — байт-в-байт то же значение, что через миг после первого
успешного раунда. Дисамбигуатор `ever_succeeded` вычисляется в `GcHealth` и теряется, не доходя ни
до SQL, ни до Prometheus-гейджа. Естественный алерт «page if age > N while is_leader = 1» для худшего
случая не срабатывает никогда, потому что застрявший 0 не пересекает никакого положительного порога.
SQL-колонка при этом **уже** `Nullable(UInt64)`.

**Проверка заявления «фикс уже есть, но не используется» — ПОДТВЕРЖДЕНО, все четыре точки.**

1. Дисамбигуатор вычисляется. `Gc/CasGcScheduler.cpp:392-407`:
```cpp
    const UInt64 last_ms = last_success_ms.load(std::memory_order_relaxed);
    h.ever_succeeded = last_ms != 0;
    if (last_ms != 0)
    {
        ...
        h.last_success_age_seconds = now_ms > last_ms ? (now_ms - last_ms) / 1000 : 0;
    }
```
Поле объявлено на `Gc/CasGcScheduler.h:126` (`bool ever_succeeded = false;`), рядом честный
комментарий `:128`: `UInt64 last_success_age_seconds = 0;   /// seconds since the last led round (0 if never)`.
Обратите внимание: при `last_ms == 0` ветка `if` вообще не исполняется, т.е. поле остаётся своим
дефолтным нулём — «никогда не лидировал» и «успел прямо сейчас» неотличимы именно так, как заявлено.

2. Ни один продакшн-потребитель `ever_succeeded` не существует. `grep -rn "ever_succeeded" src/`
даёт РОВНО четыре попадания: два в самой схеме/вычислении (`CasGcScheduler.cpp:398`,
`CasGcScheduler.h:126`) и два в тесте (`src/Disks/tests/gtest_cas_gc_log.cpp:475` —
`EXPECT_FALSE(h0.ever_succeeded);` и `:484` — `EXPECT_TRUE(h1.ever_succeeded);`). То есть поле
проверено тестом и не прочитано ни одной рендер-поверхностью — «present but unused» буквально.

3. SQL-колонка уже Nullable и NULL уже используется рядом.
`src/Storages/System/StorageSystemContentAddressedMounts.cpp:54`:
```cpp
{"last_success_age_seconds", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt64>()), "Seconds since this disk's GC last led a round (0 if it never led). NULL on rows describing other servers' mounts."},
```
Вставка безусловна для локальной строки — `:201`: `col_last_success->insert(health->last_success_age_seconds);`,
а на peer-строках уже идёт `col_last_success->insertDefault();` (`:206`), что для Nullable-колонки и
есть NULL (комментарий `:195-196` объясняет почему). Т.е. механика NULL уже развёрнута, нужный
`if (!health->ever_succeeded) col_last_success->insertDefault(); else ...` — одна строка. Побочное
наблюдение, которого в находке нет: после такого фикса NULL станет нести два смысла (peer-строка и
«ни разу не преуспел»), но они различимы соседними колонками (`is_leader`/`pending_reclaim` не NULL
только на локальной строке), так что перегрузка безвредна — и всё равно её стоит проговорить в
описании колонки, иначе описание `:54` («0 if it never led») станет ложным.

4. Prometheus-гейдж тоже отдаёт двусмысленный ноль и даже документирует его.
`src/Interpreters/ServerAsynchronousMetrics.cpp:389-390`:
```cpp
    new_values[fmt::format("CASGCLastSuccessAgeSeconds_{}", name)] = { health->last_success_age_seconds,
        "Seconds since this process last completed a successful content-addressed GC round as leader on the disk (0 if it has never led one)." };
```
Ни `CASGCEverSucceeded_<disk>`, ни пропуска гейджа при `!ever_succeeded` нет (`grep -rn
"EverSucceeded" src/` — ноль).

**Почему дубликат.** `BACKLOG/operability-and-introspection.md:480-489` (раздел
`{#gc-health-zero-is-ambiguous}`, перенос 2031-triage CAS-098) содержит этот пункт первым, с теми же
якорями (`Gc/CasGcScheduler.cpp:398`, поле на `:126`), тем же выводом («an alert of the shape
`CASGCLastSuccessAgeSeconds_<disk> > threshold` can NEVER fire») и тем же предложением фикса
(«render `NULL` (and skip the metric) when `!ever_succeeded`… the alternative is to expose
`ever_succeeded` alongside»). `fable-review-triage.md:478` M13(б) ссылается на тот же пункт
бэклога. Т.е. земля уже адъюдицирована как P2, не до релиза.

**Что опус-формулировка добавляет к сиблингу.** Опус явно фиксирует, что *нужный тип уже стоит* —
колонка уже `Nullable(UInt64)`, — т.е. переводит пункт из «нужно расширить схему наблюдаемости» в
«одна строка на существующей поверхности, миграции схемы не требуется». CAS-098 предлагает фикс, но
не отмечает, что схема к нему уже готова; именно это делает пункт дешёвым. Плюс опус формулирует
худший случай точнее — не «алерт не может сработать вообще», а «лидер (`is_leader = 1`), у которого
ВСЕ раунды падают, неотличим от здорового», т.е. называет ту самую комбинацию, для отлова которой
метрика и существует.

**Что осталось.** (а) `StorageSystemContentAddressedMounts.cpp:201` — вставлять NULL при
`!health->ever_succeeded` и поправить описание колонки `:54`; (б)
`ServerAsynchronousMetrics.cpp:389` — либо не публиковать гейдж до первого успеха, либо добавить
`CASGCEverSucceeded_<disk>`; (в) тест: `gtest_cas_gc_log.cpp:475/484` уже пинает `ever_succeeded` на
уровне структуры, но рендер-поверхности им не покрыты — нужна ассерция на NULL в
`05007_cas_gc_introspection.sh` или его соседе.

## M5 (подтверждено, P2) {#m5}

**Подтверждено дословно: строки таксономии 3 и 5b бросают `NETWORK_ERROR` мимо тормоза, все четыре top-level вызова `fetchSelectedPart` идут с `allow_ca_relink = true` по умолчанию, счётчика попыток или настройки-выключателя нет, а на двух репликах shuffle — no-op, так что постоянный отказ confirm циклится без деградации в байтовый fetch.**

**Что заявлено.** Тормоз рекурсии (`allow_ca_relink = false`) ограничивает ОДИН вызов, а не петлю
ретраев. Строки 1, 2, 5 возвращают `nullptr` и оба in-file fallback-сайта корректно рекурсируют с
`allow_ca_relink = false` — тормоз работает. Строки 3 («confirm не доказал источник») и 5b («promote
вернул `Unresolved`») БРОСАЮТ `NETWORK_ERROR`, исключение уходит в очередь репликации, та
пере-исполняет запись как СВЕЖИЙ top-level вызов с `allow_ca_relink` обратно в дефолтном `true`. Все
четыре call-site используют дефолт; счётчика попыток / circuit breaker через исполнения очереди нет;
настройки, позволяющей оператору выключить relink, нет. Итог: НАСТОЙЧИВЫЙ (не транзиентный) отказ
confirm циклится вечно, никогда не деградируя в байтовый fetch, который источник мог бы обслужить.
Liveness, не safety. Topology scoping: при ≥3 репликах shuffle каждый раз выбирает нового отправителя
и тройка-специфичный отказ самолечится (недостижимо); при 2 репликах петля пропускает себя, остаётся
один кандидат, shuffle — no-op, отправитель тот же каждый раз (достижимо).

**Что на HEAD.** Подтверждается целиком, включая scoping.

1. Тормоз и его область. `src/Storages/MergeTree/DataPartsExchange.cpp:895-905` — комментарий
«THE RECURSION BRAKE (B66b). `allow_ca_relink=false` is what bounds this… it must be spelled out at
EVERY same-sender fallback», лямбда `fall_back_to_byte_fetch` на `:906-914` действительно передаёт
`/*allow_ca_relink=*/ false` (`:913`); второй сайт — `:994-1008` (zero-copy fallback), тоже `false`
(`:1008`). Т.е. половина находки «тормоз работает для nullptr-строк» верна.

2. Асимметрия зафиксирована в коде КАК НАМЕРЕНИЕ, а не как упущение.
`DataPartsExchange.cpp:940-944`:
```
    /// ... A `nullptr` means the mechanism cannot work but the sender still has the part, so the byte
    /// re-request below is sound; a THROW means the source did not prove the binding, and the whole
    /// point of it being a throw is that this fallback must NOT run for it.
```
Т.е. «throw пролетает мимо fallback» — сознательное решение. Но заявление находки в другом: тормоз
не переживает *пере-исполнение записи очереди*, и про это код молчит.

3. Строки 3 и 5b бросают `NETWORK_ERROR`. Таксономия в комментарии:
`DataPartsExchange.cpp:1314-1321` (строка 3: «an `unproven` answer, an absent answer cookie, a
transport failure, a timeout. All one outcome… Action: THROW a locally generated retry-later
`NETWORK_ERROR`… never `nullptr`, because a byte re-request goes back to the very source whose state is
in doubt») и `:1337-1345` (строка 5b: «Action: THROW the retry-later `NETWORK_ERROR`, as row 3 --
returning `nullptr` is the one thing that must not happen, because a byte fetch would publish the part
a SECOND time over a relink that may already be committed»). Сами `throw` — `:1549` (после
`tryLogCurrentException` про confirm) и `:1568` (`case CaRelinkPromote::Unresolved`).

4. Все четыре top-level call-site идут с дефолтом. Сигнатура: `DataPartsExchange.h:129`
`bool allow_ca_relink = true);`, определение `DataPartsExchange.cpp:649`. Вызовы
`fetcher.fetchSelectedPart` вне самого `DataPartsExchange.cpp`:
`StorageReplicatedMergeTree.cpp:3483` (REPLACE PARTITION), `:3611` (clone to detached), `:5823`
(`fetchPart`), `:6009` (`replaced_disk`-путь) — ни один не передаёт последний аргумент, т.е. каждый
top-level fetch снова предлагает relink.

5. Ни счётчика попыток, ни настройки. `grep -rn "relink" src/Core/Settings.cpp
src/Storages/MergeTree/MergeTreeSettings.cpp
.../ContentAddressed/ContentAddressedSettings.cpp` — **ноль** попаданий: оператор не может выключить
relink ни глобально, ни на диске. `allow_ca_relink` — только параметр функции, не настройка;
ничего вроде «после N попыток предложить байты» в `DataPartsExchange.cpp` нет.

6. Topology scoping подтверждён. Оба цикла выбора отправителя шаффлят:
`StorageReplicatedMergeTree.cpp:5211-5217` (`findReplicaHavingPart`) и `:5391-5397`
(`findReplicaHavingCoveringPartImplLowLevel`), в обоих над `std::shuffle(replicas.begin(),
replicas.end(), thread_local_rng);` стоит комментарий «Select replicas in uniformly random order»; в
обоих внутри цикла есть `if (replica == replica_name) continue;` (`:5397+`), т.е. на двух репликах
кандидат ровно один и shuffle ничего не решает. Вывод находки (≥3 — недостижимо, 2 — достижимо)
верен.

**Дубликат?** Не полностью. Ближайшие сиблинги:
- `BACKLOG/replication.md:23` `{#relink-fallback-unknown-format-version}` (2031-triage CAS-043) — про
  то, КАКИЕ исключения доходят до fallback (`CORRUPTED_DATA`-only catch в
  `ContentAddressedMetadataStorage.cpp:2259-2272`). Тот же общий класс «throw вместо деградации», но
  другой источник и другое следствие; про переживание тормоза через очередь там ничего.
- `BACKLOG.md:558-562` (`{#issue-2233-followups}`) — прямо называет «refusal storm» «the known,
  designed `{#relink-confirm-busy-lane}` behavior (all four remediations there still open — the
  per-ref rule-3 refinement is the availability fix)». Это ровно сердцевина M5 (постоянный отказ
  confirm ⇒ повторяющийся `NETWORK_ERROR`), плюс полевое измерение: 112 598 отказов за 90-минутный
  soak, пик 9 219/мин. Но: секция с якорем `{#relink-confirm-busy-lane}` в дереве ОТСУТСТВУЕТ (обе
  ссылки висячие), так что «отслеживается» тут — на честном слове.

**Что опус-формулировка добавляет.** Сиблинги смотрят на этот класс как на *доступность/шум*
(шторм отказов, уровень логов — issue #2219, наблюдаемость confirm). Опус называет структурную
причину: тормоз рекурсии — свойство ОДНОГО вызова, а не записи очереди, поэтому у системы нет
состояния, в котором она могла бы решить «хватит, возьми байтами»; и добавляет topology scoping,
объясняющий, почему это не всплывает на трёхнодовом тестовом кластере (2 реплики — канонический HA,
и именно там shuffle не спасает).

**Что осталось.** (а) сохранять счётчик top-level попыток relink для (part, source) — например в
записи очереди или в in-memory карте `StorageReplicatedMergeTree` — и после N попыток вызывать
`fetchSelectedPart` с `allow_ca_relink = false`; заметить, что для строки 5b это НЕЛЬЗЯ делать
слепо (байтовый fetch поверх возможно закоммиченного relink — ровно то, что запрещает комментарий
`:1342-1344`), значит фикс безопасен только для строки 3, а для 5b нужен сначала resolve
неопределённости; (б) оператор-ручка выключения relink; (в) как минимум — задокументировать
асимметрию (nullptr-строки заторможены, throw-строки нет) рядом с таксономией, которая сегодня
исчерпывающе разбирает safety и молчит про liveness; (г) восстановить или заново написать секцию
`{#relink-confirm-busy-lane}` — сейчас на неё ссылаются, а её нет. P2: liveness-дефект, достижимый в
самой типовой топологии, но backoff очереди предотвращает spin, а safety-утверждения таксономии
держатся.

## M6 (подтверждено, P2) {#m6}

**Обе половины живы на HEAD: сырой код ошибки ClickHouse по-прежнему возвращается из `DisksApp::main` как POSIX-статус (усечение до 8 бит, коды кратные 256 дают `exit 0`), а `clickhouse-disks.md` не описывает ни контракт кода выхода, ни пять новых CAS-подкоманд.**

**Что заявлено.** Раньше `clickhouse-disks --query "..."` всегда возвращал `EXIT_OK`; теперь любое
исключение любой команды даёт ненулевой выход для всех пользователей `--query`. Референсная
документация инструмента не тронута — ни пять новых подкоманд, ни смена кода выхода. Дополнительно:
сырой код ошибки ClickHouse (диапазон 0–1008) возвращается прямо как POSIX-статус и маскируется до
8 бит, т.е. коды, кратные 256, становятся `exit 0` (`256 PARTITION_ALREADY_EXISTS`,
`512 SET_NON_GRANTED_ROLE`, `768 CANNOT_EXECUTE_PROMQL_QUERY`), а любой код ≥256 отдаёт искажённый
статус (`S3_ERROR` 499 → 243). Заявленная цель изменения — гейтинг CI/cron на `cas-fsck`, т.е. ровно
тот случай, который никогда не должен молча сообщать успех.

**Что на HEAD.** Подтверждается полностью.

`programs/disks/DisksApp.cpp:618-624`:
```cpp
    /// Non-interactive runs surface a failing command as a nonzero process exit (CI/cron gating,
    /// e.g. `cas-fsck` reporting dangling objects). Interactive sessions are unaffected.
    if (query.has_value() && last_command_exit_code != 0)
        return last_command_exit_code;
    return Application::EXIT_OK;
```
Возвращается именно `last_command_exit_code`, а не 1: `programs/disks/DisksApp.cpp:238`
(`last_command_exit_code = code;`, где `code = err.code()`), `:255` и `:260`
(`last_command_exit_code = ErrorCodes::STD_EXCEPTION;`). `STD_EXCEPTION` = 1001
(`src/Common/ErrorCodes.cpp:671`), т.е. `1001 & 0xFF = 233`. Указанные кратные 256 коды существуют
именно с теми номерами: `src/Common/ErrorCodes.cpp:220` `M(256, PARTITION_ALREADY_EXISTS)`, `:419`
`M(512, SET_NON_GRANTED_ROLE)`, `:650` `M(768, CANNOT_EXECUTE_PROMQL_QUERY)`. Поле объявлено в
`programs/disks/DisksApp.h:95` и сбрасывается в 0 на каждый ввод (`DisksApp.cpp:218`), т.е. при
многокомандном `--query "a; b"` наверх уходит код ПОСЛЕДНЕЙ упавшей подкоманды — деталь, которой в
находке нет и которая тоже стоит документирования.

Достижимость `exit 0` на HEAD остаётся нулевой, и честная оценка обзора («none of the three
exact-zero codes looks reachable from a disks sub-command today») подтверждается: все броски пяти
новых команд идут одним кодом `BAD_ARGUMENTS` = 36 (`src/Common/ErrorCodes.cpp:45`) — в
`programs/disks/CommandFsck.cpp:45,49,52,146,153,164,175` (в т.ч. hard-findings путь
«`cas-fsck: {} reachable object(s) MISSING (INV-NO-LOSS violation)`», `:147`), и `grep -o
"ErrorCodes::[A-Z_]*"` по `CommandFsck.cpp`/`CommandCaGcRebuild.cpp`/`CommandCaDropMember.cpp` даёт
ровно `BAD_ARGUMENTS`. Т.е. это латентный fail-open плюс действующий баг искажения статуса (любое
исключение из самого пула — `S3_ERROR` 499 → 243, `CORRUPTED_DATA`, `UNKNOWN_FORMAT_VERSION` — уже
искажается), а не сегодняшняя дыра в гейтинге.

Документационная половина подтверждена дословно. `docs/en/operations/utilities/clickhouse-disks.md` —
73 строки, разделы `Program-wide options`, `Lazy initialization`, `Default Disks`,
`Clickhouse-disks state`, `Commands`; про код выхода — ни слова (`grep -n "exit"` — ноль). Список
команд (`:36-73`) содержит `cd/copy/current_disk_with_path/help/move/remove/link/list/list-disks/
mkdir/read/read-bitmap/switch-disk/write/sed/read-checksums` и НЕ содержит ни одной из пяти CAS-команд,
которые существуют в `programs/disks/`: `CommandFsck.cpp` (`cas-fsck`), `CommandCaInspect.cpp`,
`CommandCaGcRebuild.cpp`, `CommandCaGcDryRun.cpp`, `CommandCaDropMember.cpp`. `git log -3` по этому
файлу показывает только merge-коммиты, т.е. содержательно он не менялся.

**Дубликат?** Первая половина — да: `fable-review-triage.md:549` **m10** (подтверждено, P3) фиксирует
ровно смену контракта (`DisksApp.cpp:238`, `:622-623`, коммит
`f85cb4330c8` «clickhouse-disks: non-interactive runs exit nonzero on a failed command») и отмечает,
что релиз-нота не написана; трекинг — `BACKLOG.md:190` `{#disks-exit-code-upstream}` как
carve-out-обязательство («it **rides in the CAS pull request for now**… must later be carved out into
its own upstream PR»). Замечание по гигиене трекинга: этот пункт отправляет читателя за деталями в
`docs/superpowers/cas/upstream.md` («The record lives with the carve inventory, not here»,
`BACKLOG.md:198`), а такого файла в дереве нет — ссылка висячая.

Вторая и третья половины (усечение до 8 бит / `exit 0` и отсутствие документации на пять подкоманд +
контракт кода выхода) в m10 отсутствуют и нигде не отслеживаются.

**Что опус-формулировка добавляет к сиблингу.** m10 читает изменение как breaking change для
`set -e`-скриптов, т.е. вопрос совместимости и релиз-ноты. Опус берёт другое следствие того же кода:
возвращается не «1», а сырой код ClickHouse, поэтому механизм гейтинга технически неверен независимо
от совместимости — статус искажается для всех кодов ≥256 и в принципе может стать нулевым, что
превращает «команда упала» в «команда прошла» именно в CI-гейтинге, для которого правку и делали.
Плюс опус называет вторую цену молчания документации: не только контракт выхода, но и то, что пять
новых подкоманд вообще не описаны.

**Что осталось.** (а) `programs/disks/DisksApp.cpp:622-623` → `return last_command_exit_code != 0 ? 1
: 0;` (или клампить в 1–125), чтобы POSIX-статус нёс только «упало/не упало»; сам код ошибки уже
печатается в stderr (`:262-264`). (б) описать контракт кода выхода в
`docs/en/operations/utilities/clickhouse-disks.md`, включая правило «последняя упавшая подкоманда» для
многокомандного `--query`. (в) добавить в тот же файл пять CAS-подкоманд. (г) при carve-out в
upstream-PR (`{#disks-exit-code-upstream}`) везти фикс усечения вместе с самой правкой, а не после, и
починить висячую ссылку на `upstream.md`. P2: направление fail-open, фикс однострочный, и от него
зависит объявленный способ гейтинга `cas-fsck`; но сегодня недостижимого `exit 0` нет, поэтому релиз
не блокирует.

## M7 (дубликат CAS-055 + CAS-118, P2) {#m7}

**Формы кода на HEAD верны, но головное «под дефолтами КАЖДЫЙ доступ платит HEAD» на read-path не подтверждается — тёплый `CachedForLoad`-хит обслуживается без единого обращения к бэкенду; реальный остаток (ForceFresh-HEAD на файл в `createHardLink`) уже отслежен как CAS-055.**

**Заявлено (M7).** `readManifestShared` делает безусловный `backend.head(key)` до пробы `manifest_cache`; выше `getView` отдаёт удержанный view без обращения к бэкенду только при `fresh_enough = (mode == Never) || (now - validated_at) < age_seconds*1000`, а дефолт — `Mode::Always` с `age_seconds = 0`, т.е. неравенство `< 0` никогда не истинно ⇒ «под дефолтами каждый доступ падает вниз и платит HEAD».

**Что на HEAD.**

1. `HEAD` перед пробой decode-кеша — подтверждён дословно: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasManifestReader.cpp:63-66` («`HEAD` is mandatory even on a cache hit. It proves that the live reference still names an existing object and supplies the token that identifies the immutable bytes being reused»), проба кеша ниже — `:84-86`, токен входит в ключ (`:43-54`). Обоснование — INV-NO-DANGLE, промах даёт громкий `FILE_DOESNT_EXIST` (`:79-81`).

2. Арифметика `fresh_enough` формально верна (`Parts/PartFolderAccess.cpp:202-204`: `mode == Never || (now_ms_fn() - cached->validatedAtMs()) < params.validate.age_seconds * 1000ULL`, дефолты `part_folder_validate = "always"` — `ContentAddressedSettings.cpp:97`), но **несущественна**: весь этот блок входится только при `params.validate.mode != PartFolderValidate::Mode::Always` (`Parts/PartFolderAccess.cpp:197`). Под дефолтом `always` в него вообще не заходят, так что «неравенство `< 0`» — мёртвая ветка, а не причина HEAD'а.

3. **Головное следствие не воспроизводится.** Read-path идёт под `Freshness::CachedForLoad`, и для него в `getView` есть отдельный тёплый путь ВЫШЕ: `Parts/PartFolderAccess.cpp:176-190` — при совпадении `cached->manifestId() == resolved->manifest_id` возвращается удержанный view (`return cached;` на `:186`) без вызова `buildView`, т.е. без `readManifestShared` и без `HEAD`. `readManifestShared` на read-path вызывается только из `buildView` (`Parts/PartFolderAccess.cpp:270`, `:295`). Предшествующий `resolve` (`:166`) — чисто in-memory (`CasRefLedger::resolveRef`, см. CAS-118 §2). Дефолты кеша включены: `part_folder_cache_bytes = 64 MiB` (`ContentAddressedSettings.cpp:94`), `manifest_decode_cache_bytes = 128 MiB` (`:98`). Итого тёплое чтение части = ноль запросов.

4. Единственный `ForceFresh` в `ContentAddressedMetadataStorage.cpp` — `:2128` (не чтение данных); остальные — в транзакции: `ContentAddressedTransaction.cpp:339`, `:1191`, `:1602`.

**Реальный остаток.** `createHardLink` carry-forward всё ещё делает `getView(..., ForceFresh)` на КАЖДЫЙ файл источника: `ContentAddressedTransaction.cpp:1189-1191`. Под дефолтным `always` это обязательный `HEAD` манифеста на файл части. Мемоизация «одно доказательство на (транзакцию, ref)» существует только на пути `unlinkFile` — `ContentAddressedTransaction.cpp:1595-1603` (`force_fresh_validated_refs`, коммит `a60bfde9700` «cas: one ForceFresh body proof per (transaction, ref) on the fast-removal path»), и на `createHardLink` не распространена. Это ровно CAS-055 (подтверждено, P2, `BACKLOG/performance.md:287`, где прямо цитируется `Pool/CasManifestReader.cpp:63-65` и фиксируется, что остаток — «the `HEAD` round trip, not a re-decode»).

**Чем opus-формулировка отличается от сиблингов.** CAS-055 смотрит на сторону ЗАПИСИ (`createHardLink`), CAS-118 — на дублирование parse+route+getView на одно открытие. Opus M7 добавляет только одно: явную арифметику дефолтов `Mode::Always` + `age_seconds = 0` как доказательство «HEAD-skip недостижим», и предлагает по существу новое направление фикса — дефолт `Mode::Age` с ненулевым `age_seconds` либо ограничение HEAD-skip областью запроса/транзакции. Первое (смена дефолта на `Age`) — это ослабление INV-NO-DANGLE-проверки на read-path и требует отдельного решения; второе — уже реализованный на `unlinkFile` приём, который стоит распространить на `createHardLink` (это и есть фикс CAS-055).

**Фиксящего коммита нет** — форма кода не менялась в заявленной части; изменилось только то, что головного эффекта на read-path не было и на дату обзора (тёплый `CachedForLoad`-хит присутствует).

## M8 (подтверждено (частично дубликат fable n1 + fable M12/12a), P2) {#m8}

**Все три канала утечки внутреннего происхождения на HEAD воспроизводятся, причём opus впервые даёт масштаб: ~239 строк с тегами `B<число>` в 74 файлах, включая ~35 файлов ОБЩЕГО апстрим-кода вне CAS-каталога.**

**Заявлено (M8).** Три канала: (a) ~135 тегов `B<число>` в ~50 файлах, включая общий апстрим-код (`src/IO/ReadBufferFromS3.cpp` «B117», `src/IO/S3/Client.cpp` «B166», `src/IO/S3Defines.h` «B118», `src/IO/S3/PocoHTTPClient.cpp` «see B118», `src/IO/WriteSettings.h` «RFC cas-s3-timeout-retry-control»); (b) 7 ссылок на `docs/superpowers/…` — путь, которого нет в поставляемом дереве, в том числе в ПОЛЬЗОВАТЕЛЬСКОЙ доке и в общем апстрим-хедере; (c) `B11` внутри комментария колонки живой системной таблицы.

**Что на HEAD — (a) теги, подтверждено, все пять названных мест дословно:**
- `src/IO/ReadBufferFromS3.cpp:357` — «Stop retrying once the query is cancelled (B117)»;
- `src/IO/S3/Client.cpp:112` — «downstream SYSTEM SYNC REPLICA (B166)»;
- `src/IO/S3Defines.h:39` — «(B118). The default is DISABLED…»;
- `src/IO/S3/PocoHTTPClient.cpp:636` — «(see B118)», плюс не названный обзором `src/IO/S3/PocoHTTPClient.h:93` — «(B118)»;
- `src/IO/WriteSettings.h:81` — «(RFC cas-s3-timeout-retry-control)».

Перепись на HEAD (`git grep -cE '(^|[^0-9A-Za-z])B[0-9]{1,3}([^0-9A-Za-z]|$)' -- src programs`, минус явные ложные срабатывания — `gtest_async_loader` (имена job'ов), `gtest_coordination_storage` (нумерация Keeper-кейсов), `Core/UUID.h` (hex)): **74 файла, 239 строк** — то есть шире, чем «~135 в ~50». Из них вне CAS-каталога и вне `src/Disks/tests` — 35 файлов, и это именно ОБЩИЙ код: `src/Common/ThreadStatus.h:91` («use-after-free, B90»), `src/Interpreters/ThreadStatusExt.cpp:132`, `:148` («(B90)»), `src/Common/ProfileEvents.cpp:787` («B168 P0»), `src/Disks/DiskObjectStorage/DiskObjectStorage.h:52-53` («зero-copy subsystem (B1)… honest capability — B31»), `DiskObjectStorageTransaction.{cpp:432,h:107}` («B58/B63», «B59»), `src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h:156` и `src/Disks/IDiskTransaction.h:142` («(B59)»), `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:419,540,541,581` («B34», «B21», «B36»), `DataPartStorageOnDiskFull.cpp` (8 строк, «B59»), `DataPartsExchange.cpp` (3), `IDataPartStorage.h`, `MergeTask.{cpp,h}`, `MergeProjectionPartsTask.cpp`, `MutateTask.cpp`, `MergeTreeDeduplicationLog.cpp`, `StorageReplicatedMergeTree.cpp` (2). Это прямое нарушение стоячего правила «комментарий не цитирует планы/задачи/ревью» — и, отдельно, правила «не добавлять CAS-специфику в общий Replicated/Keeper-код».
В `docs/en` тегов `B<число>` нет вообще (единственные совпадения — hex-дампы в `docs/en/interfaces/specs/NativeFormat.md`), так что часть заявления «ships in … user docs» относится не к тегам, а к каналу (b).

**(b) ссылки `docs/superpowers/…` — подтверждено, стало 7 попаданий в 6 файлах** (`git grep -n "docs/superpowers" -- src programs docs/en`):
- ПОЛЬЗОВАТЕЛЬСКАЯ дока: `docs/en/antalya/cas/architecture/correctness.md:21` («The full model index … lives at `docs/superpowers/models/`») и `:25` (заголовок таблицы);
- общий апстрим-хедер: `src/Storages/StorageTableProxy.h:62` — «tracked in docs/superpowers/cas/BACKLOG.md»;
- `programs/disks/CommandCaGcRebuild.cpp:20` — «see docs/superpowers/cas/04-gc-protocol.md#gc-rebuild» (файл удалён консолидацией 2026-08 — ссылка битая даже на ветке разработки);
- `src/.../ContentAddressed/Gc/CasGc.cpp:1569` — на `BACKLOG.md`;
- `src/.../ContentAddressed/Tools/CasFsck.h:239` — на `AGENTS.md`;
- `src/Disks/tests/gtest_cas_parallel_commit.cpp:11` — на `docs/superpowers/sdd`, каталог удалён (`e8ecc2c5bdc`).
Это ровно fable M12/12a (подтверждено, P2), где уже зафиксировано, что суть не «путь не существует», а «публичная страница адресует читателя в дерево разработки, которого он не получит», и что `docs/superpowers/` в релизную ветку не едет по построению.

**(c) `B11` в комментарии колонки живой системной таблицы — подтверждено, не менялось:** `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:47` — `{"manifests_deleted", …, "Owner-removed manifest bodies physically deleted this round (counted separately from blob deletes, B11)."}`; дубль в POD-хедере `ContentAddressedGarbageCollectionLog.h:37`. Виден через `system.columns.comment` и `DESCRIBE TABLE`. Это дубликат fable n1 (подтверждено, P3), который называет ещё два места того же класса: `programs/disks/CommandFsck.cpp:147` («INV-NO-LOSS violation» в тексте исключения) и `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:427` («not supported on a CAS disk yet (B16/B34)»).

**Чем opus-формулировка отличается от сиблингов.** fable n1 фиксирует три точечных места утечки тегов в пользовательские строки; fable M12/12a — ссылки на дерево разработки. Opus M8 сводит это в один класс и добавляет то, чего у обоих нет: **масштаб и локализацию по общему апстрим-коду** — тег не в CAS-каталоге, а в `src/IO`, `src/Common`, `src/Storages/MergeTree`, `src/Disks/DiskObjectStorage`, т.е. в файлах, которые читают люди, вообще не работающие с CAS. Отсюда и вывод обзора, который стоит принять: содержательное обоснование почти везде стоит рядом с тегом, поэтому фикс — удаление тега, а не его расшифровка.

**Фиксящего коммита нет.** Что осталось: (1) сплошной проход по 74 файлам с удалением тегов `B<число>`/`RFC …` при сохранении причины — начиная с 35 файлов общего кода; (2) снять 5 ссылок `docs/superpowers/*` из `src/`/`programs/` и переписать `correctness.md:21,25` без путей в дерево разработки; (3) убрать `B11` из комментария колонки (в `cas_gc_log.md` та же мысль уже сформулирована без тега). Всё механическое, поведения сервера не меняет — поэтому P2 и не pre-release.

## M9 (подтверждено, P3) {#m9}

**Подтверждено: 29 дисковых настроек CAS не содержат ни одной ручки verbosity/sampling, сток пишет каждое событие без фильтрации, и ожидаемый row-rate не описан ни в `cas_log.md`, ни в `monitoring.md`.**

**Заявлено (M9).** Гран лога — на решение (одна строка на PUT блоба, на dedup-adopt, на retire-решение; перечисление `CasEventType` ~50 значений), а не на запрос. Ни дисковой, ни глобальной настройки, которая семплирует/тротлит/снижает verbosity, кроме удаления всей секции `<cas_log>`. Иллюстративная граница по собственным per-round бюджетам кода (`gc_round_graduation_budget` = 5000, `gc_round_redelete_budget` = 5000 при `gc_interval_sec` = 60) — десятки тысяч строк в час на диск, плюс writer-side строки, помноженные на intra-part fan-out загрузок. `monitoring.md` не обсуждает рост хранения самого лога.

**Что на HEAD.**

1. **Гран «на решение» — подтверждён.** `Primitives/CasEvent.h`, `enum class CasEventType` — 48 значений от `BlobPut`/`BlobReuseAdopt`/`BlobRetire` до `RefResolve`/`ReadMissing`/`Exception`, т.е. счёт «~50» верен. Событие эмитится в точке решения (пример: `Pool/CasManifestReader.cpp:69-78` — `CasEventType::ReadMissing` на каждый промах).

2. **Ни одной ручки verbosity/sampling — подтверждено.** `ContentAddressedSettings.cpp:71-100` содержит ровно 29 `DECLARE(...)`; ни одна не относится к логу (перечислены: `scratch_path`, `gc_*`, `blob_hash*`, `skip_access_check`, `deduplication_*`, `manifest_sweep_*`, `part_folder_*`, `manifest_decode_cache_bytes`, `gc_meta_pool_size`, `staging_backend`, `server_root_id`, `gcs_max_token_producing_put_bytes`). Сток тоже не фильтрует: `ContentAddressedMetadataStorage.cpp:564-596` (`makeCasEventSink`) — единственные два раннихвыхода это `!context` (`:567`) и `!log` (`:576`); дальше КАЖДОЕ событие безусловно превращается в `ContentAddressedLogElement` и уходит в `SystemLog`. Ни по типу события, ни по `outcome`, ни по вероятности отбора ничего не отсекается.

3. **Уточнение к формулировке «кроме удаления всей секции».** Строго говоря, доступны стандартные `SystemLog`-ручки, и они прямо в поставляемом `config.xml`: `programs/server/config.xml:1201-1213` (`flush_interval_milliseconds` 7500, `max_size_rows` 1048576, `buffer_size_rows_flush_threshold` 524288) и закомментированный пример TTL на `:1210-1212` («example of a retention policy; disabled by default like every other log's TTL»); то же для `<cas_gc_log>` на `:1321-1329`. Но это границы ХРАНЕНИЯ и буфера, а не объёма записи: сам поток строк они не уменьшают. То есть суть претензии стоит, а её буквальная формулировка немного пережата.

4. **Обе таблицы поставляются ВКЛЮЧЁННЫМИ по умолчанию** — комментарий над секцией: `config.xml:1197-1200` («Enabled by default while the feature is experimental … Remove the section to disable»). Отдельно отмечу связанное расхождение, уже зафиксированное как fable n7: `src/Interpreters/ContentAddressedLog.h:12` до сих пор пишет «Optional (off by default); enabled for soak/CI», что прямо противоречит поставляемому конфигу.

5. **Документация роста — подтверждено с одной поправкой.** `docs/en/operations/system-tables/cas_log.md` не содержит ни оценки row-rate, ни слова о росте/TTL (grep по `volume|row|grow|ttl|retention|disable` даёт только `:17` про грануляцию `cas_gc_log` и `:39` про `LowCardinality`). В `docs/en/antalya/cas/operations/monitoring.md` есть ровно одна пограничная фраза — `:27`: «two are ordinary `system.*_log` tables and follow the usual flush/retention settings» — но ни ожидаемой интенсивности, ни правила «прикидки на глаз», ни привязки к профилю нагрузки нет. Так что «never discusses the log's own storage growth» — почти верно: упоминание ручек есть, обсуждения объёма нет.

6. **Иллюстративная арифметика обзора воспроизводится:** `gc_round_graduation_budget = 5000` (`ContentAddressedSettings.cpp:84`), `gc_round_redelete_budget = 5000` (`:85`), `gc_interval_sec = 60` (`:74`) — это верхние границы деструктивной работы раунда, и каждая единица этой работы имеет свой тип события; при 60 раундах в час десятки тысяч строк на диск достижимы без экзотики. Мягчащий фактор, который обзор сам называет, тоже верен: обычная асинхронно-буферизованная семантика `SystemLog` и нулевая цена без сконфигурированного CAS-диска.

**Не дубликат по существу.** Ближайший ранее адъюдицированный сосед — CAS-104 (`BACKLOG/operability-and-introspection.md:533`), но он про другое: сток УСТАНОВЛЕН даже при удалённой секции `<cas_log>`, поэтому событие строится и выбрасывается (цена CPU при выключенном логе). Про сам ОБЪЁМ записи при включённом логе в `2031-triage.md` пункта нет (grep по `verbos`/`sampl`/row-rate — пусто). Единственное частичное покрытие — `BACKLOG/performance.md` {#standalone-write-scratch-manifest-cost}, где отмечено, что «объём audit-строк лечится независимо: одна агрегированная строка класса `BlobReuseAdopt` на публикацию со счётчиком», т.е. записан один конкретный класс агрегации, а не общая ручка.

**Что осталось (P3, операбельность, без риска корректности).** (1) Дописать в `cas_log.md`/`monitoring.md` ожидаемую интенсивность с правилом прикидки (строки на вставленную часть × число блобов + строки на GC-раунд) и явно указать TTL как рекомендуемую практику для этих двух таблиц. (2) Рассмотреть дисковую настройку verbosity, отсекающую не-аномальные исходы (`outcome == "success"` для объёмных классов `BlobPut`/`BlobReuseAdopt`/`RefResolve`), оставляя аномалии всегда — сток для этого уже единая точка (`ContentAddressedMetadataStorage.cpp:573`). (3) Заодно исправить неверный комментарий `src/Interpreters/ContentAddressedLog.h:12` («off by default»).

**Фиксящего коммита нет** — ни настройки, ни фильтрации, ни документации объёма на HEAD не появилось.

## M10 (подтверждено, P3) {#m10}

**«Ноль продакшн-вызывающих» проверено точно и подтверждается: ни одной конструкции и ни одной записи в реестре вне `src/Disks/tests`, при этом 598 строк fault-injection безусловно попадают в `dbms` через каталожный glob.**

**Заявлено (M10).** `Backend/CasInMemoryBackend.{h,cpp}` — 598 строк fault-injection (`failNextCasPut`, `setEnforceTokens(false)`, симулированные delete-маркеры) — попадают в `dbms` через безусловный `add_headers_and_sources(dbms .../Backend)`, при нуле вызывающих вне `/tests/`; контраст с соглашением, которое корректно исключает 147 `gtest_cas_*.cpp` через glob `gtest*.cpp`. Фикс — перенести в `src/Disks/tests/`.

**Проверка «ноль вызывающих» на HEAD — точная.**

1. **Размер совпадает буквально:** `wc -l` → `CasInMemoryBackend.h` 172 + `CasInMemoryBackend.cpp` 426 = **598**.

2. **Конструкции.** `git grep -n "InMemoryBackend" -- src programs`, за вычетом самого класса и `src/Disks/tests/`, даёт ровно **три попадания, и все три — текст комментария**, ни одной конструкции и ни одного объявления переменной:
   - `Backend/CasBackend.h:299` — «(…e.g. `InMemoryBackend`)» в описании контракта;
   - `Gc/CasGcShardPlan.h:81` — «sealed run back over an `InMemoryBackend`»;
   - `Pool/CasPlainObjects.cpp:120` — «deterministic instead of relying on `InMemoryBackend` ordering»;
   плюс перечисление в `ContentAddressed/README.md:95`.
   Единственный продакшн-подобный сосед, который РЕАЛЬНО конструируется, — `InstrumentedBackend`: `Pool/CasPool.cpp:372` и `:809` (`backend = std::make_shared<InstrumentedBackend>(std::move(backend));`), т.е. поправка обзора про четыре «настоящих» класса верна.

3. **Реестр.** Ни одной записи: `git grep -n "in_memory\|inmemory"` по каталогу `ContentAddressed` и по `MetadataStorageFactory.cpp` — пусто. Backend выбирается не по имени из конфига, так что «строкой в конфиге» его тоже не поднять.

4. **Test-only использование — единственное.** Все вызывающие лежат в `src/Disks/tests`: подключение в общем хелпере (`src/Disks/tests/cas_test_helpers.h:11`), три производных фейк-бэкенда над ним (`CountingBackend` — `:1401`; `HintHoleBackend` — `:1739-1740`; `MetaWriteFaultBackend` — `:1953`) и прямые конструкции в gtest'ах (`gtest_ca_dedup_cache.cpp:66,76,86,100,138,173,194`, `gtest_ca_wiring.cpp:2788-2790,2839,2897`, `gtest_cas_b140_dangle.cpp:19-21,66`, `gtest_cas_backend.cpp:6,124` и др.).

5. **Бенчмарк тоже не пользователь.** Единственный не-gtest-потребитель CAS в дереве — `ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp` (подключается через `add_subdirectory` на `src/CMakeLists.txt:144`), и `grep "InMemoryBackend"` по нему пуст. То есть никакой «стал использоваться» не произошло.

6. **Механизм попадания в бинарь подтверждён, номер строки сместился:** glob-и лежат на `src/CMakeLists.txt:135-142`, и Backend-каталог забирается строкой **`:138`** — `add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend)` (в обзоре указано `:135`, это строка родительского каталога после переезда `592b9b83568`). Контраст с gtest-соглашением тоже верен: `src/CMakeLists.txt:901-908` — `grep_gtest_sources` собирает `gtest*.cpp` в отдельную цель `unit_tests_dbms`, т.е. тестовый код в `dbms` не попадает — а этот файл именем `gtest*` не начинается и потому попадает.

7. **Fault-injection-поверхность подтверждена дословно:** `CasInMemoryBackend.h:18-19` (докблок: «`failNextCasPut`: inject a one-shot conflict», «`setEnforceTokens(false)`: mimic a "dumb" backend that ignores token checks»), объявления — `:112` (`void failNextCasPut(const String & key);`), `:127` (`void setEnforceTokens(bool enforce);`), состояние — `:166` (`std::set<String> fail_next_cas_;`), `:169` (`bool simulate_delete_markers_ = false;`).

**Что осталось.** Ровно предложенный обзором перенос: `Backend/CasInMemoryBackend.{h,cpp}` → `src/Disks/tests/` рядом с `cas_test_helpers.h` (все реальные потребители уже там), либо — если файл хочется оставить на месте — исключить его из `dbms`-glob'а явно. Блокирующих зависимостей нет: три упоминания в продакшн-коде текстовые, реестра нет, бенчмарк не использует. P3: не поведение, а Ockham/поверхность бинаря — но фикс дешёвый и снимает из релизного бинаря класс «умышленно ломающий бэкенд».

**Фиксящего коммита нет.** Последние коммиты по файлу — `b967100a6ff`, `a3554bd696d`, `56e0294c095` (resurrect-семантика), размещения не касались.

## M11 (подтверждено, P2) {#m11}

**Подтверждено: ни настройки `allow_experimental_*`, ни какого-либо эквивалента нет; практический гейт сегодня — только строка в конфиге диска `<metadata_type>cas</metadata_type>` плюс проза в `docs/en/antalya/cas/index.md`, причём при регистрации диска не печатается ни одного предупреждения.**

**Заявлено (M11).** ~50 настроек `allow_experimental_*` гейтят куда меньшие фичи, CAS не гейтит ни одна. При регистрации `cas`-диска не срабатывает никакого предупреждения. Единственный гейт — строка конфига `<metadata_type>cas</metadata_type>`, а оговорка «experimental, format may change» живёт только в прозе, которую пользователь должен уже знать, что надо прочитать.

**Что на HEAD.**

1. **Настройки-гейта нет.** `grep "allow_experimental" src/Core/Settings.cpp` даёт 82 попадания (обзор говорил «~50» — на HEAD их даже больше), плюс 11 в `src/Storages/MergeTree/MergeTreeSettings.cpp`; ни одно из них не относится к CAS (`grep "allow_experimental" src/Core/Settings.cpp | grep -iE "cas|content"` — пусто). Эквивалента на серверном уровне тоже нет: в `src/Core/ServerSettings.cpp` из CAS-специфичного объявлен только `cas_blob_upload_pool_size` (`:152`) — ручка размера пула, не гейт.

2. **Регистрация диска молчит.** `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:217-244` — `registerContentAddressedMetadataStorage` регистрирует тип `"cas"` (`:219`) и сразу конструирует `ContentAddressedMetadataStorage` (`:240-242`); ни `LOG_WARNING`, ни проверки настройки, ни аргумента «acknowledge» в этой функции нет. Более того, слово «experimental» вообще не встречается ни в одном файле каталога `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (`grep -rni "experimental"` — пусто), то есть у сервера в рантайме нет ни одной строки, которая сообщала бы оператору статус фичи.

3. **Практический гейт сегодня** — ровно три вещи, и все три слабые:
   - **Конфиг-строка.** Диск нужно объявить с `metadata_type = cas`. Это опт-ин на диск, но опт-ин ровно того же вида, что и любой поддерживаемый тип метаданных: никакой дополнительной отметки «я понимаю, что это эксперимент» не требуется.
   - **Проза.** `docs/en/antalya/cas/index.md:69-73`, секция `## Status`: «`CAS` is **experimental**. It ships in Altinity Antalya builds. Experimental means the on-disk format and the SQL surface can still change between releases…». Это единственное место, где статус зафиксирован для пользователя, и оно на странице, до которой надо дойти.
   - **Сборка.** Косвенный, но реальный барьер: фича поставляется в билдах Altinity Antalya (там же, `index.md:69`), а не в апстрим-сборках. Это ограничивает аудиторию, но не является гейтом внутри бинаря.
   Единственное упоминание статуса, которое реально попадает в поставку рядом с настройками, — комментарий в `programs/server/config.xml:1197-1200`: «Enabled by default while the feature is experimental». То есть в конфиге статус упомянут только применительно к включённому по умолчанию `cas_log`, а не к самому диску.

4. **Привилегии — не тот гейт.** SQL-поверхность закрыта грантами (7 CAS-привилегий), но это контроль ДОСТУПА, а не признание экспериментальности; к тому же по fable n3 ни одна из них не описана в `docs/en/sql-reference/statements/grant.md`.

**Что осталось (P2, compat/операторская безопасность).** Минимальный вариант — при регистрации `cas`-типа печатать однократный `LOG_WARNING` («experimental: on-disk format and SQL surface may change between releases; no compatibility scaffolding») в `MetadataStorageFactory.cpp:219`, чтобы факт попадал в лог сервера и в отчёт об инциденте. Полный вариант — обычный для дерева гейт вида `allow_experimental_content_addressed_storage` (или серверная настройка/флаг диска-подтверждения), отказывающий в монтировании без явного опт-ина. Решение о форме гейта — продуктовое: pre-release-позиция «формат меняется дёшево, без compat-scaffolding» (та самая `index.md:71-73`) как раз и есть аргумент ЗА жёсткий гейт, потому что она означает, что данные на CAS-диске между релизами могут не пережить апгрейд.

**PRE-RELEASE: нет** — не P1: сегодняшний опт-ин на диск не даёт CAS включиться самому, а Antalya-сборка ограничивает аудиторию; но пометить до релиза стоит именно потому, что после первого «настоящего» релиза добавление гейта станет ломающим изменением конфигов.

**Фиксящего коммита нет** — ни настройки, ни предупреждения на HEAD не появилось; заявление обзора верно целиком, поправка только в счёте (`allow_experimental_*` не ~50, а 82 + 11).

## M12 (подтверждено, P2) {#m12}

**Подтверждено: единственный дренаж — шов `mutateRefsAfterWriterCleanup` перед очередной durable ref-мутацией того же namespace; ни GC-раунд, ни монтирование, ни FSCK, ни фоновый publisher, ни teardown его не дренируют — teardown только НАБЛЮДАЕТ долг.**

**Заявлено (M12).** `drainWriterCleanupDuties` имеет ровно одного вызывающего — шаблон `mutateRefsAfterWriterCleanup` (6 call site'ов, все — durable ref-мутации). Ни фонового, ни периодического, ни GC-, ни shutdown-дренажа нет. На teardown долг только наблюдается (`const bool drained = ref_lanes_drained && !writerCleanupDutiesPending();`). Следствие 1: namespace, у которого сорвался publish и который больше не получает durable ref-мутаций, держит долг бесконечно; `~PartWriteTxn` намеренно оставляет build активным ⇒ `min_active` не двигается ⇒ `prefixEligible` ложен для каждого следующего build-префикса на этом server root ⇒ неограниченное удержание manifest-debris. Следствие 2: незаслуженное «нечистое» прощание ухудшает путь восстановления сукцессора при каждом последующем рестарте.

**Что на HEAD — граф вызовов ровно как заявлено (номера строк новые).**

1. **Один вызывающий дренажа.** `git grep "drainWriterCleanupDuties" -- src` даёт объявление (`Pool/CasPool.h:1056`), определение (`Pool/CasPool.cpp:1297`) и ЕДИНСТВЕННЫЙ вызов — `Pool/CasPool.h:1065`, внутри `mutateRefsAfterWriterCleanup`. Контракт зафиксирован в комментариях дословно: `Pool/CasPool.h:1052-1053` («Drain `ns` before admitting its next ordinary mutation») и `:1059-1061` («The single admission seam for durable ref mutations exposed by `Pool`»).

2. **Шесть точек допуска, все — durable ref-мутации:** `Pool/CasPool.cpp:1661` (`dropRef`), `:1670` (`updateRefPublishedAt`), `:1678` и `:1686` (две перегрузки `dropNamespace`), `:1722` (`appendRefOps`), `:1731` (`tryPublishSnapshotAndAdvanceCheckpointOnce`).

3. **Другого пути дренажа НЕ появилось — проверено по каждому кандидату:**
   - **GC-раунд: нет.** `grep` по `Gc/*.cpp` на `appendRefOps|dropRef|dropNamespace|updateRefPublishedAt|tryPublishSnapshot` — ни одного попадания. GC ходит по namespace'ам, но не через шов `Pool`.
   - **FSCK: нет.** То же по `Tools/*.cpp`: единственное попадание — `Tools/CasDecommission.cpp:185-194` (`admin->dropNamespace(life)`). То есть дренаж случайно происходит только при операторском декоммиссии дохлого участника, и только для его namespace'ов.
   - **Фоновый publisher снапшотов: нет.** `Pool::tryPublishSnapshotAndAdvanceCheckpointOnce` (со швом) продакшн-вызывающих вне тестов не имеет; фоновый поток `CAS_REF_SNAPSHOT_PUBLISH` зовёт внутренний `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` напрямую (`Pool/CasRefLedger.cpp:4010`, диспетчер `:3993-4015`), обходя обёртку `Pool` и, значит, дренаж.
   - **Монтирование/remount: нет.** Ни `writer_cleanup*`, ни `drainWriterCleanupDuties` не встречаются вне `Pool/CasPool.{h,cpp}` (`git grep "writer_cleanup" -- src`, за вычетом тестов, даёт только эти два файла).
   - **Фонового потока/периодики нет вовсе:** нет ни `ThreadName`, ни таймера, связанного с этой очередью.
   - **Teardown: только наблюдение, подтверждено дословно** — `Pool/CasPool.cpp:893` (в `~Pool`) и `:956` (в пути `SYSTEM CAS FORGET`): `const bool drained = ref_lanes_drained && !writerCleanupDutiesPending();`. Дренируются только ref-полосы (`ref_ledger.drainRefLanesForShutdown`), долг — нет.
   Итого: **«только следующая мутация того же namespace» на HEAD держится**, с единственной добавкой — операторский `dropNamespace` в декоммиссии.

4. **Механизм пина floor'а подтверждён по цепочке.** `PartWriteTxn::~PartWriteTxn` (`Pool/CasPartWriteTxn.cpp:124-140`): при `precommit_state == Uncertain || Durable` — `store->enqueueWriterCleanupDuty(...)` и **`return`** без `retireBuildSeq` (комментарий `:126-130`: «Transfer that exact cleanup duty to the mount and KEEP the build active»); `retireBuildSeq` вызывается только на ветке «долга нет» (`:139`). Дальше: `CasMountRuntime::minActive()` — `active_build_seqs.empty() ? next_build_seq : *active_build_seqs.begin()` (`Pool/CasMountRuntime.cpp:150-154`), т.е. неснятый seq пинит floor; `minActive` штампуется в heartbeat mount-lease (`Pool/CasMountRuntime.cpp:236-241`); а `prefixEligible` (`Gc/CasOrphanManifestSweep.cpp:475-493`) при равном `writer_epoch` требует `w.min_active > prefix.build_sequence` (`:493`), и `sweepNamespace` при неeligible удаляет НОЛЬ (`:499-501`). Так что удержание manifest-debris — реальное следствие, и оно **server-root-широкое** (floor берётся из mount-lease server root'а), а не только для сорвавшегося namespace.

5. **Уточнение к слову «неограниченное».** Удержание ограничено ЖИЗНЬЮ ПРОЦЕССА: `prefixEligible` сначала сравнивает эпохи (`:485-489`, «old-epoch debris drains after a process restart even when its build_sequence is above the current min_active»), поэтому после рестарта весь debris прошлой эпохи становится eligible. Точная формулировка — «удержание на всё время работы процесса и для всей его эпохи», а не «навсегда». Это тот же гейт, который уже адъюдицирован в CAS-077 ({#dead-member-frozen-build-floor}), но там источник заморозки floor'а другой — непереклеймленный слот мёртвого узла; здесь узел ЖИВ, а floor пинит собственный недренированный долг. Не дубликат.

6. **Второе следствие подтверждено, и у него есть добавочный дефект диагностируемости.** `mount_runtime.finishTeardown(drained)` при `drained == false` намеренно не пишет terminal-маркер: `Pool/CasMountRuntime.cpp:505-531` — «If draining did not certify that every in-flight PUT resolved, a clean farewell would be false evidence. Stop background renewal without a terminal operation so the successor uses the slower but safe observation-based reclaim path» (`:526-528`); при `drained == true` — `mount_keeper->stop()` с farewell `min_active = UINT64_MAX` (`:507-509`), который сукцессор трактует как немедленную reclaim-годность. То есть «незаслуженное прощание меняет путь восстановления» — не только комментарий, а прослеженная развилка. **Добавка к находке:** WARNING в этой ветке (`:529-530`) утверждает «CAS store shutdown with an unresolved ref-log PUT», хотя причиной может быть именно неразряженный writer-cleanup долг при полностью дренированных ref-полосах — то есть оператор получает неверную первопричину.

**Что осталось (P2).** Предложение обзора правильное и остаётся невыполненным: добавить триггер дренажа, не зависящий от повторной записи в namespace. Естественное место — GC-раунд (он и так обходит каждый namespace), либо шаг перед teardown-наблюдением в `~Pool`/`FORGET` (сейчас там ровно наблюдение). Плюс мелочь: развести в WARNING'е `Pool/CasMountRuntime.cpp:529-530` две разные причины нечистого прощания. Класс — удержание (retention) и деградация пути восстановления, не потеря данных, поэтому не pre-release; но P2, потому что один сорвавшийся publish на затихшем namespace тормозит уборку по ВСЕМУ server root'у до рестарта.

**Фиксящего коммита нет.**

## Blast radius — детали {#tiers-details}

### T1 (исправлено, P3) {#t1}

Перепроверка не переделывалась — подтверждаю корректность кросс-ссылок `opus-review-triage.md` {#b1} и `fable-review-triage.md` {#m9} (фикс `faab6678d8f`). На HEAD `grep -rn "gcs_conditional_dialect\|usesGcsConditionalDialect" src/` даёт 0 вхождений, а адаптация запроса/ответа гейтится по запросу: `src/IO/S3/PocoHTTPClient.cpp:760` (ответ), `:911` и `:1021` (запрос) — все под `isNativeConditionalRequest(request)`. Контракт зафиксирован в `src/IO/S3/GCSConditionalDialect.h:11-13`. Не-CAS трафик через тот же клиент идёт как `Default` байт-в-байт по upstream-пути, т.е. Tier-1-регрессия снята. Остаток P3 — живой прогон на GCS и релиз-нота про новую строгость `gcs_hmac`, но это уже не blast radius.

### T2 (подтверждено, P2) {#t2}

Подтверждаю `opus-review-triage.md` {#m6} дословно на HEAD: `programs/disks/DisksApp.cpp:622-623` — `if (query.has_value() && last_command_exit_code != 0) return last_command_exit_code;`, а в `last_command_exit_code` кладётся сырой код ClickHouse (`:238` `= code`, `:255`/`:260` `= ErrorCodes::STD_EXCEPTION` = 1001 → `& 0xFF` = 233). Коды, кратные 256, существуют (`PARTITION_ALREADY_EXISTS` 256, `SET_NON_GRANTED_ROLE` 512, `CANNOT_EXECUTE_PROMQL_QUERY` 768) и дали бы `exit 0` — латентный fail-open ровно в том сценарии (CI/cron-гейтинг `cas-fsck`), для которого изменение и делалось. `docs/en/operations/utilities/clickhouse-disks.md` по-прежнему не содержит ни слова про exit code (`grep -n exit` → 0) и не упоминает ни одну из пяти новых `cas-*` подкоманд. Это единственная строка блока, затрагивающая пользовательский контракт вне CAS и не имеющая ни документации, ни релиз-ноты.

### T3 (подтверждено, P3) {#t3}

Подтверждаю `opus-review-triage.md` {#m1}. На HEAD `src/IO/S3Common.h:96-99` — `return error.GetResponseCode() == PRECONDITION_FAILED || error.GetExceptionName() == "PreconditionFailed" || error.GetMessage().find("PreconditionFailed") != npos;`, и `src/IO/S3/Client.cpp:113` — `if (S3::isPreconditionFailedError(error)) return false;` внутри `Client::RetryStrategy::ShouldRetry`, до проверок `attemptedRetries`/`isQueryCanceled`/`error.ShouldRetry()`. То есть подавление ретрая по подстроке из тела ответа действует для бэкапов, Iceberg, `s3queue` и обычных S3-дисков. Комментарий над предикатом (`S3Common.h:70-95`) после обзора переписан честно (message-fallback объявлен), сам дизъюнкт не тронут. Направление политики 412 остаётся улучшением; риск — только over-match на транзиентной ошибке.

### T4 (частично, P2) {#t4}

Подтверждаю `fable-review-triage.md` {#m10}. Статика на HEAD: `src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:405` и `:419` — `if (has_shared_transaction) return;` в `beginTransaction`/`commitTransaction`; `src/Storages/MergeTree/MergeTreeData.cpp:9374` и `:9396` — закрытие дисковой транзакции части в `Transaction::renameParts`/`commit` под `hasActiveTransaction()`; `:5968` и `:8739` — те же гейты. Дефолты не-CAS не тронуты (`IDataPartStorage.h` `isContentAddressed() → false`, `IDiskTransaction.h` `tryGetInFlight* → {}`). Централизации нет: `PartTransactionScope` в дереве отсутствует, правило по-прежнему выводится заново примерно в десятке мест. Записи «прогнали апстримные stateless/stress/integration полосы и дельты против базы нет» в `docs/superpowers/cas/` нет ни одной — это самая широкая по охвату строка блока (все таблицы MergeTree), и обязательство остаётся доказательственным, а не кодовым.

### T5 (подтверждено, P3) {#t5}

`src/IO/ReadBufferFromS3.cpp:361` — `if (CurrentThread::isInitialized() && CurrentThread::get().isQueryCanceled()) return false;` в `processException`, т.е. на пути каждого S3-чтения, а не только CAS. Изменение согласовано с уже существующей upstream-проверкой в SDK-стратегии (`src/IO/S3/Client.cpp:119`, та же строка есть в базе `4b7cecaa3cf`), поэтому это выравнивание внешнего цикла с внутренним, а не новая политика. Наблюдаемая дельта для не-CAS: чтение, которое ранее завершалось после сигнала отмены (пройдя backoff), теперь прерывается — killed-запрос больше не «зомбирует» минутами. Ни `CHANGELOG.md`, ни `docs/en/` про это ничего не говорят; трекинг только как carve-out в upstream. Риск считаю низким: направление — фикс, и оно уже было полу-реализовано апстримом.

### T6 (частично, P3) {#t6}

Дифф против базы (`git diff 4b7cecaa3cf HEAD -- src/Storages/MergeTree/MergeTreeDeduplicationLog.cpp`) показывает замену `chassert(current_writer != nullptr);` на бросок в двух местах: `MergeTreeDeduplicationLog.cpp:281-287` (`addPart`) и `:325-331` (`dropPart`), оба `ErrorCodes::LOGICAL_ERROR`. Дальше в апстримном коде шёл `writeRecord(record, *current_writer)` (`:296`), так что «тихого прохода» не было — был segfault в release; новое поведение строго лучше. Достижимость на не-CAS низкая: конструктор создаёт `logs_dir` при `deduplication_window != 0` (`:97-98`), а `rotateAndDropIfNeeded` (`:240`) при `!disk_supports_writing_with_append` всегда вызывает `rotate()` и создаёт writer; ранний выход `load()` (`:103-117`) теперь пропускает `Plain` и `CAS`, т.е. окно с null-writer сузилось. Два дефекта гигиены: код `LOGICAL_ERROR` для условия, зависящего от конфигурации диска (per [[feedback_logical_error_tests_death_split]] это скорее не-`LOGICAL_ERROR`), и комментарий `:278-279` ссылается на «the release-build chassert above», которого в файле больше нет.

### T7 (подтверждено, P3) {#t7}

`src/Common/ThreadStatus.h:87-93` объявляет `ThreadGroupPtr parent_thread_group` с объяснением UAF, и оба дочерних конструктора его заполняют: `src/Interpreters/ThreadStatusExt.cpp:133` (borrowed child, из `createForMaterializedView`, `:241`) и `:149` (`createForFlushAsyncInsertQueue`, `:254`). Значит для НЕ-CAS сервера экспозиция тоже есть — через группы материализованных представлений и flush async-insert, а не только через CAS. Следствие подтверждается механикой апстрима: пик логируется в деструкторе трекера уровня `Process` (`src/Common/MemoryTracker.cpp:183-187`, `log_peak_memory_usage_in_destructor` по умолчанию `true`), а деструктор родительской группы теперь наступает после смерти последней дочерней. Практический масштаб задержки на не-CAS мал (группы MV/flush обычно живут в пределах запроса); длинные хвосты ~90-120 с — это CAS-специфичный `operation_deadline_ms`. Сам фикс корректен, ничего в `CHANGELOG.md`/`docs/en/` про смену времени логирования нет.

### T8 (подтверждено, P3) {#t8}

На HEAD `src/Disks/.../ContentAddressed/Pool/CasBlobUploadPool.cpp:36-38` — `if (size == 0) throw Exception(ErrorCodes::BAD_ARGUMENTS, "cas_blob_upload_pool_size must not be 0");`, и вызов безусловен из `programs/server/Server.cpp:1735` и `programs/local/LocalServer.cpp:438` (плюс `programs/disks/DisksApp.cpp:560` с константой 16). То есть строка обзора воспроизводится дословно. Смягчающее обстоятельство, которого в находке нет: описание настройки прямо говорит «Zero is rejected: the pool must have at least one thread» (`src/Core/ServerSettings.cpp:152-156`) — причём эта формулировка была уже в дереве обзора (`git show 056488b47a0:src/Core/ServerSettings.cpp:151-155`), т.е. это документированный fail-loud, а не сюрприз. Достижимость требует, чтобы администратор сам выставил 0 CAS-настройки на не-CAS сервере; риск считаю P3, но пункт стоит завести в BACKLOG (сейчас он не отслеживается нигде).

### T9 (частично, P3) {#t9}

Секции на месте и без всякого гейта: `programs/server/config.xml:1201-1213` (`<cas_log>`, с комментарием «Enabled by default while the feature is experimental … Remove the section to disable») и `:1321-1330` (`<cas_gc_log>`). Механика создания: `src/Interpreters/SystemLog.cpp:634-643` — `prepareTable()` вызывается только если `!result.logs.empty()` либо `result.create_table_force`, а форс приходит только из `SystemLogs::flush` с `should_prepare_tables_anyway = true` (`:534-536`, путь `SYSTEM FLUSH LOGS`). Значит на не-CAS сервере таблицы не появляются при старте, но появляются после любого `SYSTEM FLUSH LOGS` — что и делает почти вся тулинг/тесты, поэтому практический вывод обзора («сюрприз для тулинга, диффящего `system.tables`») в силе. Реальная безусловная цена, которой в находке нет: два `SystemLog`-объекта поднимают по потоку сохранения на каждом сервере (`src/Common/SystemLogBase.cpp:303`). Документация есть: `docs/en/operations/system-tables/cas_log.md:19-20` прямо говорит «it is enabled by default in the shipped `config.xml`»; в `CHANGELOG.md` — ничего.

### T10 (частично, P3) {#t10}

`git diff 4b7cecaa3cf HEAD -- src/Disks/DiskObjectStorage/ObjectStorages/Local/` даёт ровно 6 добавленных строк. Весь переписанный `LocalObjectStorage::listObjects` (явный стек `directory_iterator`, `isVanishedEntryError`, symlink-guard, проверка встроенного NUL) уже есть в базе `4b7cecaa3cf` — он был принят апстримом (`7b89d9e0786` «Merge pull request #111483 from ClickHouse/iceberg_local_fix», ветка `altinity/backports/antalya-26.6/111483`). Единственная branch-local дельта — `LocalObjectStorage.cpp:432-436`: в `tryStatResolvedPath` каталог теперь возвращает `nullopt` вместо броска `fs::file_size` «Is a directory» (комментарий прямо ссылается на обход `system.remote_data_paths` по CA-пулу). Это влияет только на `LocalObjectStorage::tryGetObjectMetadata` (`:470-474`); бросающий `getObjectMetadata` (`:455`) не тронут, а `listObjects` вызывает хелпер только для не-каталогов. Т.е. формулировка обзора «it changes listing semantics on a shared path» на HEAD больше неверна; остаётся микро-расширение контракта «try»-функции. Ни changelog, ни доков.

### T11 (подтверждено, P3) {#t11}

Хелпер на месте: `src/Interpreters/InterpreterSystemQuery.cpp:283-288` (`unwrapTableProxy`), с 11 точками применения (`:1402`, `:1479`, `:1688`, `:2166`, `:2212`, `:2284`, `:2725`, `:2871`, `:2885`, `:2912`, `:2936`). Fan-out-точки по-прежнему сознательно не разворачивают (`:1653`, `:1749`, `:2248`, `:2747` — все через `it->table()`), что корректно: разворот там материализовал бы каждую ленивую таблицу. Однако одно-табличный путь НЕ полон: в `trySyncReplica` цикл разворота материализованного представления делает `dynamic_cast<StorageMaterializedView *>(table.get())` без разворота прокси (`:2160`), и только последующий каст к `StorageReplicatedMergeTree` идёт через `unwrapTableProxy` (`:2166`) — значит `SYSTEM SYNC REPLICA` по ленивому MV не увидит MV. Отдельная безусловная дельта в общем коде: `src/Storages/StorageProxy.h:150-157` добавляет форвардинг `checkMutationIsPossible` в nested, что затрагивает не только `StorageTableProxy`, но и `StorageTableFunctionProxy` (`src/Storages/StorageTableFunction.h:25`) — там `getNested()` инстанцирует вложенное хранилище до отказа. Направление изменения — фикс (раньше `SYSTEM`-глагол по ленивой таблице врал «Table is not replicated»), но в `CHANGELOG.md`/`docs/en/` про смену поведения `lazy_load_tables` нет ничего.

### T12 (исправлено, —) {#t12}

`git diff 4b7cecaa3cf HEAD -- ci/jobs/scripts/check_style/various_checks.sh` пуст, а `git diff 056488b47a0 HEAD` по тому же файлу показывает `26 deletions` — проверку сняли. Коммит `eee9a2b8a11` («ci: drop the triple-quote SQL style check from the shared style script», 2026-08-05) в сообщении прямо объясняет: «It rode into the CAS branch after a CAS test tripped the class it detects, but it is a generic check in a shared upstream file … The check itself is kept aside untracked for a standalone upstream submission». Соответственно риск «упасть на предсуществующих нарушениях в чужих местах дерева» снят полностью, CI-поверхность ветки по этому файлу нулевая.

## Minor m1-m16 — детали {#minor-1-details}

### m1 (подтверждено, P3) {#m1}

На HEAD `src/IO/ReadBufferFromFileView.cpp:117-122` выполняет `impl->seek(new_pos, SEEK_SET)` внутри `executeWithOriginalBuffer` (сам swap уже восстанавливается на исключении, `:143-156`), затем `:124-129` бросают `SEEK_POSITION_OUT_OF_BOUND`, и только `:131-132` делают ребейз `file_offset_of_buffer_end = impl_buffer_end; resizeWorkingBuffer();`. То есть выход по исключению оставляет вью с офсетом до seek при переставленном `impl`, и последующий `read`/`seek(SEEK_CUR)` посчитает `current_position` (`:107`) неверно. Оба потребителя вью — `src/IO/PackedFilesReader.cpp:102` и `src/IO/ReadPipeline.cpp:699` — исключение из `seek` не глушат, так что сегодня это латентно; починка тривиальна (ребейзить до bound-check либо в `catch`).

### m2 (дубликат CAS-019, P2) {#m2}

Полностью совпадает по основанию с `docs/superpowers/cas/2031-triage.md` {#cas-019} (`Parts/PartFolderAccess.cpp`, ключ single-flight `ns+ref`). Отдельной проверки не требует, вердикт и приоритет наследуются.

### m3 (частично, P3) {#m3}

`Gc/CasGcScheduler.cpp:65-73`: `if (thread.joinable()) return; ... thread = ThreadFromGlobalPool(...); hb_thread = ThreadFromGlobalPool(...);` — если вторая конструкция бросит (исчерпание глобального пула), первый поток уже запущен, а повторный `start()` вернётся сразу. На пути монтирования (`ContentAddressedMetadataStorage.cpp:841-848`) вреда нет: `scheduler` там — локальный `shared_ptr`, его деструктор при раскрутке зовёт `stop()` и джойнит оба потока (это прямо задокументировано в комментарии `:834-840`). Остаётся путь `GC START` (`ContentAddressedMetadataStorage.cpp:1029-1040`), где инстанс уже сохранён в члене `gc_scheduler` и переживает бросок; последствие ограничено — heartbeat advisory, а кража lease безопасна по дизайну (разбор TTL/heartbeat — `2031-triage.md:272`).

### m4 (дубликат CAS-050, P2) {#m4}

Ровно то же основание, что в `2031-triage.md` {#cas-050}: `Gc/CasGcScheduler.cpp:77-85` берёт `mutex` только под `stopping`, а `:95-104` читает `thread.joinable()` под `mutex`. Вторая половина находки («joinable-but-dead планировщик») там же признана невоспроизводимой.

### m5 (дубликат fable {#m8}, P3) {#m5}

`ContentAddressedMetadataStorage.cpp:884-895`: внутри `std::lock_guard ptr_lock(pointer_mutex)` делаются `gc_scheduler.reset(); part_access.reset(); cas_store.reset();`, наружу вынесен только `old_scheduler->stop()` (`:896-899`). Правильный образец рядом: `forgetDisk` (`:926-936`) копирует `pool`/`scheduler` под локом и весь протокол ведёт вне него. Починка — та же однострочная (снять указатели в локальные `shared_ptr`, разрушать после выхода из области лока).

### m6 (частично, P3) {#m6}

`Pool/CasRefLedger.cpp:4289` — `const RefTableSnapshot snap = snapshotOf(candidate_state, ns.string());` стоит МЕЖДУ захватом копии (`:4245-4263`, под `state_mutex`) и явным разрушением копии под локом (`:4302-4305`, с большим комментарием `:4291-4301`, что каждая межпоточная копия создаётся И разрушается под state-локом). `snapshotOf` (`Pool/CasRefProtocol.cpp:610-615`) бросает `CORRUPTED_DATA` на не-`Live` lifecycle, но этот случай уже отсечён `hasStateBearingSnapshotCandidateUnderStateLock` под тем же локом (`:4256`), поэтому реально остаётся только `bad_alloc` на `reserve`/`push_back` (`:4621-4627`) плюс, формально, бросок из `runtime_still_admitted()` (`:4281`) и тестового хука (`:4274`). Последствие ровно то, от чего защищается комментарий: релиз-декремент `use_count` шаредных баз без happens-before с `materializeCommitted` (TSan-репортабельно). Дешёвая страховка — `SCOPE_EXIT`/`try-catch`, разрушающий `candidate_state` под локом.

### m7 (исправлено, —) {#m7}

На головe `grep -rn "SingleWriterSlot" src` пуст; в `Pool/CasServerRoot.h` единственное упоминание `string_view` — `#include <string_view>` (`:19`). Класс существовал на ревьюируемом коммите (`git show 056488b47a0:.../Pool/CasServerRoot.h`, поля `std::string_view slot_name; std::string_view terminal_verb;`) и удалён коммитом `ecf3d5d7c76` «refactor: centralize CAS mount renewal ownership» (`CasServerRoot.h`: −313/+…, оба `string_view`-члена удалены). Хрупкий по конструкции паттерн исчез вместе с классом.

### m8 (дубликат fable {#m6}, P2) {#m8}

`Pool/CasBlobUploadPool.cpp:51-58` — `ThreadPool & blobUploadPool() { std::lock_guard lock(pool_mutex); if (!pool_instance) throw …; return *pool_instance; }`, `:60-64` — `shutdownBlobUploadPool()` = `pool_instance.reset()`. Единственный продовый потребитель прежний: `ContentAddressedTransaction.cpp:334` — `Cas::fanOutBlobUploads(*st.build, requests, Cas::blobUploadPool());`, ссылка держится на весь submit-and-join. Настоящее закрытие — `shared_ptr`/счётчик использований, как записано в fable M6.

### m9 (подтверждено, P3) {#m9}

`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h:168` — `bool supportsRetryProfile(ObjectStorageRetryProfile) const override { return true; }` (параметр даже не назван), тогда как база фейлит закрыто: `IObjectStorage.h:437` — `return profile == ObjectStorageRetryProfile::Default;`. Перечисление сейчас — `{Default, SingleAttempt}` (`src/IO/WriteSettings.h:17-21`), потребитель один: `Backend/CasObjectStorageBackend.cpp:121-126`. Соседний по смыслу `supportsCopyMode(mode)` в том же классе объявлен с ИМЕНОВАННЫМ параметром и реализован по значению — то есть асимметрия внутри одного файла; правка на один `switch`.

### m10 (дубликат fable {#m7}, P3) {#m10}

На HEAD `Primitives/CasTypes.h:3` — `#include <.../ContentAddressed/Formats/CasFormat.h>`, использование одно: `:160` `fmt::format("{:06}{}", manifest_ordinal, storedSuffix(FormatId::PartManifest))`. Это единственный include из `Primitives/` наружу (проверено по `grep "include.*ContentAddressed" Primitives/*.h`). Основание совпадает с `fable-review-triage.md` {#m7} дословно.

### m11 (дубликат CAS-091, P3) {#m11}

Совпадает по основанию и по оговорке про emulated/local object storage с `2031-triage.md` {#cas-091}. Отдельной проверки не требует.

### m12 (подтверждено, P3) {#m12}

Обработчик `CAS_DROP_POOL_MEMBER` встроен прямо в `switch` (`src/Interpreters/InterpreterSystemQuery.cpp:1087-1100`) и сразу делает `getContext()->getDisk(query.disk)` без `if (disk_name.empty()) throw`. У соседей guard явно оформлен как «fail-closed backstop for a directly-constructed AST»: `runContentAddressedGcRebuild` (`:2574-2578`), `runContentAddressedFsck` (`:2613-2616`), `contentAddressedForget` (`:2648-2650`), `contentAddressedGcStop` (`:2673-2675`), `contentAddressedGcStart` (`:2696-2698`). Парсер требует диск синтаксически (`ParserSystemQuery.cpp:517-521`), так что дефект чисто в глубине защиты/сообщении.

### m13 (подтверждено, P3) {#m13}

`ObjectStorages/S3/S3ObjectStorage.cpp:960-963` — комментарий «Respect the disk's configured expect_continue_min_bytes; if unset, use the established 1 MiB floor» и `if (cfg.expect_continue_min_bytes == 0) cfg.expect_continue_min_bytes = 1024*1024;`. А `src/IO/S3/PocoHTTPClient.cpp:639-641` прямо пишет: «`0` (the default, carried by every non-CAS S3 client) DISABLES it entirely». Дефолт настройки действительно `0` (`src/IO/S3Defines.h:42`, `src/IO/S3AuthSettings.cpp:28` — причём с ПУСТЫМ описанием), то есть «выключено» и «не задано» в конфиге неразличимы, и явное `expect_continue_min_bytes = 0` у CAS-диска молча не срабатывает. Вреда нет (порог сознательный, B118), дефект — в отсутствии выключателя и в противоречии описания.

### m14 (подтверждено, P3) {#m14}

Два места: `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:585-586` (`if (params.metadata_version_to_write.has_value()) { chassert(!params.keep_metadata_version); … }`) и `src/Storages/MergeTree/MergeTreeData.cpp:10081-10082` (та же пара). Поля объявлены рядом и независимо: `src/Storages/MergeTree/IDataPartStorage.h:273,276`. Разделение call-site'ов перепроверено на HEAD: `metadata_version_to_write` ставят только `StorageReplicatedMergeTree.cpp:3455/9782/10073`, `keep_metadata_version = true` — только `StorageReplicatedMergeTree.cpp:5792`, `IMergeTreeDataPart.cpp:2552`, `MutateTask.cpp:3388`. Поле — не наше (пришло из upstream ещё в 2023, `296f9968c04`), так что превращение `chassert` в бросок затрагивает общий MergeTree-код и по политике требует консультации, а не «мелкой правки».

### m15 (подтверждено, P3) {#m15}

`src/Parsers/ParserSystemQuery.cpp:504-525`: команда не идёт через `parseQueryWithOnClusterAndTarget(..., SystemQueryTargetType::Disk)` (как соседи, напр. `:500`), а парсит `ParserStringLiteral` дважды — `:512` (srid) и `:519` (диск). Комментарий `:506-510` объясняет только srid («an opaque server-root path (may contain '/'), not the bare identifier»); для имени диска такого основания нет, и `SYSTEM CAS DROP POOL MEMBER 'srv1' FROM DISK cas_disk` сегодня — синтаксическая ошибка. Расширение до «идентификатор ИЛИ литерал» обратно совместимо, поэтому не блокирует релиз.

### m16 (дубликат CAS-035, P2) {#m16}

`Gc/CasGc.cpp:506-511`: внутри `GcPhaseTimer t(phase_sink, "defer_decision")` идёт `walk_plan.emplace(buildRefWalkPlan(listRefPrefix(state)))`, и только затем `changed = walk_plan->changedRows()` кормит `shouldDeferRound(...)` — то есть перенести LIST «после вердикта» нельзя без другой схемы обнаружения изменений. Ровно эта же пара (полное перечисление префикса с удержанием ключей + связь с отставанием `cleanupRefObjects`) уже зафиксирована в `2031-triage.md` {#cas-035} и в его же заметке к CAS-034 (`2031-triage.md:1648-1652`).

## Minor m17-m31 — детали {#minor-2-details}

### m17 (by-design, —) {#m17}

На HEAD `PartWriteTxn::ensureBlobPresent` открывает каждую попытку публикации строкой `const HeadResult head = store->backend().head(key);` (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:331`) — никакого порога по размеру в коде нет вовсе; порог 1 MiB — это `INLINE_CAP` на входе (мелкие файлы вообще не становятся блобами), поэтому формулировка обзора «для каждого блоба ≥1 MiB» фактически совпадает с «для каждого блоба». Сам код называет шаг «mandatory `HEAD`» и строит на нём наблюдение fence-поколения, adopt-backfill и различение `Absent`/`Condemned` (`:360`, `:380`, `:455-456`), то есть это не оптимизация дедупа, а линеаризующая точка протокола. Замер от 2026-08-23 подтверждает поставленную семантику дословно: «Every fan-out task issued exactly one blob `HEAD`» (`docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md:18-19`), и там же зафиксировано, что изолированная стоимость этого `HEAD` не измерена. Вердикт — by-design под СТОЯЩИМ ВЕТО (правки шага протокола как «дешёвой оптимизации» не предлагаются и не планируются).

### m18 (подтверждено, P3) {#m18}

Конструктор — `Xxh3Streamer() : state(XXH3_createState()) { XXH3_128bits_reset(state); }`, деструктор — `XXH3_freeState(state)`, член — `XXH3_state_t * state;` (`src/Disks/.../ContentAddressed/Primitives/CasXxh3Streamer.h:44,47,71`), то есть форма кода из обзора не менялась. Единственный потребитель — `Xxh3128BlobHashingWriteBuffer`, который держит `Xxh3Streamer state;` как член (`Primitives/CasBlobHashingWriteBuffer.cpp:133`) и создаётся по одному на публикуемый блоб, так что это одна дополнительная аллокация ~576 байт на блоб — величина, тонущая на фоне самого PUT. Дефолт остаётся `cityhash128`, эта ветка не берётся вовсе; чинится встраиванием `XXH3_state_t` по значению с `alignas(64)`, но выгода в пределах шума.

### m19 (подтверждено, P3) {#m19}

`DataPartStorageOnDiskBase::clonePart` при `dst_disk->isContentAddressed()` гонит весь клон через одну транзакцию приёмника — `copyDirectoryContentIntoTransaction(...); clone_transaction->commit();` (`src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:800-813`), а сама функция — обычный `for` по `iterateDirectory` с `readFile`/`writeFile`/`copyData` на файл (`:645-663`). Комментарий над ней прямо фиксирует и причину, и статус: «Sequential, not the parallel copyThroughBuffers thread pool: a content-addressed transaction batches every file into ONE eventual manifest, and its staging map is not mutex-guarded … parallelizing this remains a deferred optimization whose cost is now visible to a waiting statement rather than only to a background operation» (`:632-636`). Не-CA ветка уходит в `src_disk->copyDirectoryContent(...)` (`:820`), где работает пул `IDisk::copyDirectoryContent` — то есть асимметрия реальна; заметим, что при этом уже есть пул фан-аута блобов на пути INSERT (`cas_blob_upload_pool_size`), которого этот путь не использует.

### m20 (частично, P3) {#m20}

В `ServerAsynchronousMetrics.cpp:385-391` живут ровно четыре CAS-метрики, все про GC (`CASGCIsLeader_*`, `CASGCPendingReclaim_*`, `CASGCLastSuccessAgeSeconds_*`, `CASGCWedgedNamespaces_*`); ничего про lease там нет. Однако «no scrapeable metric» — преувеличение: `src/Common/ProfileEvents.cpp:928-938` объявляет `CASMountRenewalAttempts/Retries/Resolved/Recovered/DeadlineExceeded`, `CASMountLeaseLost`, `CASMountReleaseSkippedForeignOccupant`, `CASMountExclusivityViolation`, а SQL-поверхность даёт `renewal_sequence`/`expires_at` (`src/Storages/System/StorageSystemContentAddressedMounts.cpp:46,48`), и именно на них ссылается `docs/en/antalya/cas/operations/troubleshooting.md:20`. Отсутствует ровно per-disk async gauge того же семейства, что у GC. Вторая половина верна: `grep -rn CASBlobUploadPool docs/` даёт ноль попаданий вне текста самого обзора, при том что `troubleshooting.md:20` советует оператору понижать `cas_blob_upload_pool_size`, не назвав ни одной метрики, по которой это решение принимается.

### m21 (дубликат fable n7, P3) {#m21}

На HEAD `src/Interpreters/ContentAddressedLog.h:12` по-прежнему пишет «Optional (off by default); enabled for soak/CI», тогда как `programs/server/config.xml:1201-1213` и `:1321-1330` поставляют секции `<cas_log>` и `<cas_gc_log>` включёнными. Ровно это уже зафиксировано как fable n7 (`fable-review-triage.md:594`) и как пункт 3 «что осталось» в разборе M-строки (`opus-review-triage.md:1345`). Правка — одна строка комментария, поведения не меняет.

### m22 (дубликат fable {#m12} 12b, P2) {#m22}

Перепроверка на HEAD: все десять имён (`gc_round_{graduation,redelete,sweep_namespace,sweep_recovery_op,ref_cleanup,prefix_wholesale,handoff_prefix_wholesale,outcome_entry}_budget`, `manifest_sweep_{list,delete}_budget_keys`) дают ноль попаданий и в `docs/en/antalya/cas/configuration.md`, и во всём каталоге `docs/en/antalya/cas/`, при том что они объявлены (напр. `ContentAddressedSettings.cpp:80,82`). Числитель сместился: сейчас `DECLARE(` в `src/Disks/.../ContentAddressed/ContentAddressedSettings.cpp` — 27 штук против 29 на момент обзора, а таблица `configuration.md` даёт 21 строку, так что арифметика «10 из 29» устарела, а сам пропуск — нет. Вердикт fable (P2, не pre-release, операционный риск в инциденте: оператор не найдёт ручки темпа GC) переносится без изменений.

### m23 (частично, P3) {#m23}

Перебор всех `.md` под `docs/en/antalya/cas` даёт ровно один файл с нулём строк `^# ` — `architecture/mounts-and-leases.md`; у всех остальных ровно один H1. При этом её frontmatter содержит `title: 'CAS Architecture — Mounts and Leases'` (`docs/en/antalya/cas/architecture/mounts-and-leases.md:6`), а текст начинается сразу с абзаца «Page 4 of 4 in the CAS architecture set…» (`:11`); Docusaurus в отсутствие H1 подставляет frontmatter-title как заголовок страницы, так что визуальной «страницы без названия», скорее всего, нет. Подтверждается ровно несогласованность с 21 сестринской страницей плюс отсутствие явного якоря вида `{#…}`, требуемого проектным правилом для заголовков; правка — одна строка `# CAS architecture — mounts and leases {#mounts-and-leases}`.

### m24 (подтверждено, P3) {#m24}

Файл на месте, с полным frontmatter и корректным H1-якорем (`docs/en/antalya/cas/architecture/design-history.md:5,10`), содержит секцию «Rejected paths» с таблицей вида «What it was / Why it was abandoned / What replaced it» (`:14-16`). Поиск по `docs/en` вне `antalya/cas` не находит ни одной страницы того же жанра (три попадания на `asynchronous_metrics.md`/`metric_log.md`/`query_metric_log.md` — ложные, там слово «rejected» в другом смысле), так что уникальность жанра обзор описал верно. Смягчение, которого в находке нет: страница не «утекла» — на неё ссылаются три места книги (`docs/en/antalya/cas/index.md:88`, `architecture/index.md:114`, `roadmap.md:105`), а даты 2026-06-01…2026-08-03 в тексте нужны читателю ровно как хронология пивотов. Решение (оставить как публичный reference / убрать даты / перенести в `docs/superpowers`) — редакционное, до релиза не блокирующее.

### m25 (дубликат fable m6 (расширенный третьей таблицей), P3) {#m25}

`system.cas_gc_log` строит `type_enum`/`outcome_enum`/`trigger_enum` как `DataTypeEnum8` (`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:18,21,25`); `system.cas_log` для того же класса колонок берёт `lc_string` (`src/Interpreters/ContentAddressedLog.cpp:17`, применён к `event_type`, `disk_name` и др.); `system.cas_mounts` отдаёт `state` («live, expired, terminated, fenced or corrupt») и `lifecycle`/`lifecycle_reason` голым `DataTypeString` (`src/Storages/System/StorageSystemContentAddressedMounts.cpp:51,56,57`). Прецедент кодовой базы (`part_log.event_type`, `query_log.type`, `text_log.level`) — `Enum8`, так что расхождение реально; цена — только эргономика фильтров и размер, поведение не затронуто. Вердикт fable m6 (подтверждено, P3, не pre-release) распространяется на все три таблицы.

### m26 (подтверждено, P2) {#m26}

`grep -i "content-addressed\|content_addressed\|metadata_type = cas"` по `CHANGELOG.md` даёт ноль попаданий, и слова «antalya» в файле нет вовсе — то есть `CHANGELOG.md` в дереве это апстримный релизный changelog (26.6/26.7), собираемый из полей «Changelog entry» в описаниях PR, а не файл, который правят руками по фиче. Поэтому буквальная формулировка находки верна, но исполнимая форма фикса — не коммит в `CHANGELOG.md`, а changelog-entry в описании PR (категория «New Feature») плюс релиз-ноты Antalya. Уже есть два известных долга того же класса, которые надо свести в одну запись: строка про отказ на неизвестный ключ CAS-конфига (`BACKLOG/docs-and-cleanup.md:76`) и строка про новую строгость `gcs_hmac` к `x-amz-*` (`fable-review-triage.md:365`). Считаю P2/pre-release: без этого фича уезжает в релиз без единой пользовательской записи.

### m27 (подтверждено, P3) {#m27}

На HEAD `src/Disks/tests/cas_test_helpers.h` — 2214 строк (в обзоре 1994, то есть вырос), включён 105 из 145 `.cpp` в `src/Disks/tests/`; транзитивное замыкание — `Pool/CasPool.h` 1133, `Gc/CasGc.h` 978, `Pool/CasRefProtocol.h` 794 строки (в обзоре 1159/1012/794). Ни PCH, ни unity-сборки в дереве нет, так что каждый TU действительно парсит всё замыкание заново. Продакшн-бинарь это не затрагивает вовсе; сам обзор помечает пункт как «matches existing codebase convention», и раскол хелперов на несколько заголовков — рефактор ради времени сборки, который до релиза не нужен.

### m28 (дубликат T8, P3) {#m28}

`docs/superpowers/cas/opus-review-triage.md:36` и разбор на `:1469` воспроизводят строку дословно: `Pool/CasBlobUploadPool.cpp:36-38` бросает `BAD_ARGUMENTS` на нуле, вызов безусловен из `programs/server/Server.cpp:1735` и `programs/local/LocalServer.cpp:438`. Смягчение оттуда же: описание настройки прямо говорит «Zero is rejected: the pool must have at least one thread» (`src/Core/ServerSettings.cpp:152-156`, та же формулировка была и в дереве обзора), и это отражено в публичной доке (`docs/en/antalya/cas/configuration.md:123`). Достижимость требует, чтобы администратор сам выставил 0 CAS-настройки на не-CAS сервере — P3; отдельного BACKLOG-пункта по-прежнему нет.

### m29 (частично, P3) {#m29}

На HEAD `src/Parsers/ASTSystemQuery.h:288-292` объявляет `enum_range` с `min = 0, max = 512`, тогда как в дереве обзора (`git show 056488b47a0:src/Parsers/ASTSystemQuery.h`) стояло `max = 255` — значит формулировка «следующий контрибьютор за 256 записей» на HEAD неверна, порог теперь 512 при ~138 текущих значениях. Механизм риска сохраняется: `getTypeIndexToTypeName` резервирует `resize(magic_enum::enum_count<...>())` и индексирует по ЗНАЧЕНИЮ элемента (`src/Parsers/ASTSystemQuery.cpp:22,29`), так что выход значения за пределы окна даёт OOB-запись на статической инициализации, и ни `static_assert`, ни тест этого не ловят. Важное для скоупа: строку с `max = 512` принёс `c04e775e939` («fix(Parsers): extend magic_enum range for ASTSystemQuery::Type», Sema Checherinda, 2026-07-10), присутствующий в `remotes/altinity/antalya-26.6` — это уже не изменение CAS-ветки, и по действующему правилу «не править апстримные/общие поверхности без консультации» правка `static_assert` здесь — консультационный пункт, а не задача.

### m30 (дубликат fable m1, P3) {#m30}

`programs/disks/CommandFsck.cpp` использует `BAD_ARGUMENTS` и для CLI-ошибок (`:45`, `:49`, `:53`), и для четырёх integrity-находок (`:147`, `:154`, `:165`, `:176`), а `DisksApp.cpp:239-250` на `code == ErrorCodes::BAD_ARGUMENTS` печатает `command->options_description` (или полный список команд) до текста ошибки. То есть exit-code-контракт не различает «кривой флаг» и «пул повреждён» — ровно вывод fable m1 (`fable-review-triage.md:522`), где фикс назван тривиальным: `CORRUPTED_DATA` на четырёх сайтах. Затрагивает все пять новых команд (`CommandFsck.cpp`, `CommandCaGcRebuild.cpp`, `CommandCaGcDryRun.cpp`, `CommandCaInspect.cpp`, `CommandCaDropMember.cpp`), поведение сервера не меняет.

### m31 (подтверждено, P2) {#m31}

`tests/integration/compose/docker_compose_rustfs.yml:6` — `image: rustfs/rustfs:1.0.0-beta.12`; та же версия зашита константой в стейтлес-обвязке (`ci/jobs/scripts/clickhouse_proc.py:168` `RUSTFS_VERSION = "1.0.0-beta.12"`, скачивается релизный zip с GitHub на `:177-178`) и повторена ещё в восьми `utils/ca-soak/docker-compose*.yml`. Пин по тегу без digest: бета-тег upstream'а может быть перезалит или удалён, и тогда лейн либо тихо меняет бэкенд под собой, либо перестаёт подниматься — это риск воспроизводимости CI, а не корректности продукта (RustFS используется только как S3-совместимый бэкенд тестов, в поставку не входит). Минимальная мера до релиза — единая точка версии плюс пин по `sha256`-digest (digest образа уже зафиксирован в отчёте измерений: `sha256:612a6707053c27c41816e79e5d5d30b5ba8479fb9b500ae4908cd4a723e888fa`, `docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md:40`).

## Needs verification — детали {#nv-details}

### NV-1 (частично, P2) {#nv-1}

**Структурная часть находки подтверждается полностью и на HEAD.**

1. `merged` считается из view, взятого ДО загрузки блобов:
   `ContentAddressedTransaction.cpp:363` — `if (auto view = metadata_storage.partAccess()->getView({ns, ref}, Cas::Freshness::ForceFresh))`,
   затем `:379-381` (`stageManifest` + `precommitAdd` + `uploadPendingBlobs(st)`),
   и только потом `:384-390` строит `merged` из `view->manifest()->entries` — то есть из снимка,
   взятого до всей загрузки. Окно ровно такое, как описано в находке (и на S3 оно длинное).
2. `repointRef`'s собственный `resolve(key, Freshness::ForceFresh)` (`Parts/PartFolderAccess.cpp:555`)
   используется ИСКЛЮЧИТЕЛЬНО для побайтового short-circuit (`:565-567`); при несовпадении
   результат `resolved` больше нигде не участвует, кроме выбора уровня лога (`:574-587`).
3. `promote(allow_repoint=true)` ретайрит то, что закоммичено СЕЙЧАС, без сравнения с ожидаемым:
   `Pool/CasPartWriteTxn.cpp:876-882` — `if (const auto it = state.getCommitted().find(final_ref_name); it != ... && !(it->second.manifest_ref == id.ref)) { if (!allow_repoint) throw...; repoint_old = it->second.manifest_ref; }`.
   `repoint_old` берётся из текущего состояния, а не из ожидаемого — это и есть отсутствующее CAS-предусловие.
4. Внутрипроцессной сериализации по ref нет: в `CachedPartFolderAccess` два мьютекса —
   `inflight_mutex` (`Parts/PartFolderAccess.h:383`, только single-flight ЧТЕНИЯ, `.cpp:276-292`)
   и `explain_mutex` (`:401`, диагностика). Пишущего per-ref мьютекса нет.

**Часть про достижимость — не подтвердилась при обходе MergeTree-сайтов.**
Standalone-записи/удаления по УЖЕ закоммиченной части на одном сервере, найденные в дереве:
- `txn_version.txt` (MVCC): единственный писатель — `VersionMetadataOnDisk::storeInfo`
  (`src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp:280-283`), который берёт
  `std::lock_guard lock_storing_version{persisted_info_mutex}` и вдобавок имеет оптимистическую
  проверку `expected_storing_version != new_info.storing_version → TOO_OLD_VERSION` (`:224-231`).
  То есть по одной части записи в `txn_version.txt` сериализованы, а расхождение версий ловится.
- `metadata_version.txt`: `IMergeTreeDataPart::writeMetadataVersion` (`IMergeTreeDataPart.cpp:1756`)
  на HEAD не имеет НИ ОДНОГО вызывающего (`grep -rn writeMetadataVersion src/` даёт только
  объявление и определение) — сайт мёртв.
- `checksums.txt`/`columns.txt`: `writeChecksums`/`writeColumns` вызываются только из
  `IMergeTreeDataPart.cpp:1906` и `:2206` (одноразовые фиксапы при загрузке части) и из
  `StorageMergeTree.cpp:3375` (`CHECK TABLE`-репарация). Загрузочные фиксапы происходят до того,
  как часть становится активной.
- Поток удаления части (`delete_tmp_*`, per-file unlink) работает по обречённой части эксклюзивно
  (`BACKLOG/gc.md:42` PART-REMOVAL-REPOINT — 17707 repoint'ов, ВСЕ на `delete_tmp_*`).

**Вывод.** Data-loss я НЕ смог сконструировать: не нашёл пары конкурентных standalone-писателей в
один закоммиченный ref. Но «недостижимо» здесь держится на перечислении сайтов, а не на инварианте
(ср. `feedback_impossibility_claims_need_their_discriminator`): любой новый сайт, пишущий файл в
закоммиченную часть вне `persisted_info_mutex`, немедленно откроет lost update, и он будет ТИХИМ.
Поэтому рекомендация ревью («`repoint_old == expected` — дешёвая страховка») в силе: передавать в
`repointRef`/`promote` ожидаемый `ManifestRef` (тот, из которого построен `merged`) и падать
fail-closed при несовпадении. Это превращает тихую потерю в отказ. Приоритет P2, не блокер:
поднять до P1 только если найдётся живой конкурентный сайт.

### NV-2 (не подтвердилось, P3) {#nv-2}

Заявление таксономии («abandon отвергается state-machine'ой, если promote всё-таки приземлился»)
проверено в Pool-слое и держится ДВУМЯ независимыми уровнями:

1. **Типизация формы.** `PartWriteTxn::abandon` формирует ровно один op с
   `op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, manifest}` и без
   `new_binding` (`Pool/CasPartWriteTxn.cpp:1051-1056`). Классификатор формы даёт
   `OwnerTransitionShape::RemovePrecommit`, чья ветка в `RefTableState::applyOwnerTransition`
   работает ТОЛЬКО с картой `precommits` (`Pool/CasRefProtocol.cpp:268-279`). Карту `committed`
   она не трогает вообще — снести опубликованный ref такой op не может физически.
2. **Явная проверка отсутствия.** `if (precommits.erase({b.ref_name, b.manifest_ref}) == 0) throw
   Exception(CORRUPTED_DATA, "RefTableState: exact precommit binding '{}' to remove is absent")`
   (`Pool/CasRefProtocol.cpp:271-273`). Приземлившийся promote уже перенёс биндинг из `precommits`
   в `committed` (`OwnerTransitionShape::Promote`, `:302-306`), значит `erase` даёт 0 → отказ.

Дополнительно проверено, что на пути landed-but-`Unresolved` НЕ включается послабление
`tolerate_absent`: оно ставится только при `precommit_state == Uncertain`
(`Pool/CasPartWriteTxn.cpp:1041-1046`), а `promote` переводит `precommit_state` в `Settled` только
ПОСЛЕ успешного `appendRefOps` (`:930`); при неоднозначном append он остаётся `Durable`, то есть
берётся строгая форма. Но даже под `tolerate_absent` результат был бы тот же (пустой список op'ов),
так что оба ветвления безопасны.

Цепочка вызовов на этом пути: `PreparedPartWrite::promote` ловит бросок, `terminal` уже НЕ выставлен
(он ставится внутри allocation-free региона `promoteBuild`, `Parts/PartFolderAccess.cpp:341-348`),
идёт `abandonBuildBestEffort` (`:452`), который ловит CORRUPTED_DATA и возвращает `false`;
`PreparedRelinkOverPartWrite::promote` затем видит `write.commitIsUnresolved()` и возвращает
`CaRelinkPromote::Unresolved` (`ContentAddressedMetadataStorage.cpp:2147-2155`) — то есть «повторить
весь fetch позже, байты НЕ тянуть». `SCOPE_EXIT`-`abort()` (`:2171-2186`) снова упирается в тот же
отказ и глушит его через `tryLogCurrentException` — `noexcept` соблюдён.

**Остаточное (косметика, P3).** На корректном landed-but-`Unresolved` пути хендл остаётся
не-terminal, и деструктор `~PreparedPartWrite` печатает `LOG_ERROR` «was destroyed while still owing
its terminal operation» (`Parts/PartFolderAccess.cpp:406-415`) плюс ещё один залогированный
`CORRUPTED_DATA`. Это ложная ERROR-тревога на пути, где всё сработало правильно; для оператора
выглядит как повреждение. Стоит понизить/различить этот случай, но на безопасность не влияет.

### NV-3 (частично, P2) {#nv-3}

**(а) Перечисление — сделано, и оно исчерпывающее.** `fable-review-triage.md` V1
(`docs/superpowers/cas/fable-review-triage.md:76` и разбор `:651-659`) — обход ВСЕХ durable/
conditional write-сайтов CAS-дерева (~40 сайтов, ~19 групп). Результат: ровно ОДИН сайт
writer-плоскости без `checkFenceOrThrow` — `createNamespaceStep1` (`Pool/CasRefCatalog.cpp:200-230`),
единственный вызывающий `casUpdateImpl`, который не исполняет «the fence obligation» из
`Pool/CasRefCatalog.h:84-95`; его же вызывающий `createNamespace` (`:543-596`) держит и
`admitted_generation`, и `check_fence_or_throw` в области видимости и передаёт их в шаг 3. Фикс
тривиален и симметричен четырём другим вызывающим. Остальные сайты либо зафенсены напрямую
(`Pool/CasPartWriteTxn.cpp:362,371,383,392,445,447,464`; `ContentAddressedTransaction.cpp:926`;
`Pool/CasPool.cpp:201,221`), либо принадлежат другой плоскости с явно названным собственным гейтом.
То есть запрошенная ревью работа не «missing» — она выполнена другой полосой того же дня.

**(б) SIGSTOP-тест.** В `tests/integration` и `tests/queries` такого теста НЕТ (`grep -rn SIGSTOP
tests/integration` даёт только `test_hedged_requests` и хелперы). Но SIGSTOP-эквивалент существует и
РЕГУЛЯРНО исполняется в soak-харнессе: `utils/ca-soak/soak/chaos.py:15-21,79-85,129-137` —
`FaultAction.FREEZE_LONG` = `docker pause` (cgroup freezer = SIGSTOP для всех задач контейнера)
ровно одной CH-реплики, удерживаемый дольше `ttl(30s) + margin(ttl/2=15s)`, с последующим `unpause`
(= SIGCONT); инвариант закреплён юнит-тестом расписания `utils/ca-soak/tests/test_chaos_schedule.py:45-53`.
И главное — **живое наблюдение ровно того, что просило ревью**:
`utils/ca-soak/scenarios/BACKLOG.md:1487-1492` (2026-07-09, 83-секундный FREEZE_LONG ch1) —
размороженная реплика попыталась писать и получила `Code 236 ... CAS mount lost / lease expired —
refusing to mutate ref shard for server_root 'ca_soak_ch1'`; запись фенсенной стороны была
ОТКЛОНЕНА. Запись в BACKLOG прямо говорит: «The CA server behaved CORRECTLY (fail-closed ABORTED — a
fenced replica must not mutate); the WORKLOAD retry logic was at fault» — чинили харнесс, не сервер.
Смежное: сценарий `utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py` (потеря аренды +
remount-восстановление), `s45_decommission_hidden_removing.py:44,110` (тот же fence-margin).

**Что остаётся.** Ни один из этих прогонов не идёт в CI: FREEZE_LONG вероятностный (~1/6 upgrade,
`chaos.py:35`), карточка SIGSTOP-ack-floor в `scenarios/BACKLOG.md:674` до сих пор «proposed».
Оценка «60/100 — thinnest» справедлива именно как оценка РЕГРЕССИОННОГО покрытия, а не как «не
проверялось никогда». Закрывается детерминированным тестом (integration, `test_cas_shared_pool`):
`docker pause` держателя аренды сверх TTL, взятие аренды пиром, `unpause`, ассерт на код 236 у
in-flight записи размороженного + ассерт `CASMountLeaseLost`/`CASRemountSucceeded`.

### NV-4 (не подтвердилось, P2) {#nv-4}

Дефект, который воспроизводит `_sab_holeylist`, сформулирован в
`docs/superpowers/models/README.md:386-392`: «GC discovers ref-log transactions by a paginated LIST
with no completeness proof… the omitted `+1` sinks below its namespace cursor and is never folded
again, so GC deletes a deduplicated blob a confirmed, promoted manifest references».

**На HEAD этой предпосылки больше нет.** `Gc/CasGc.cpp:1836-1856`:
«records within one `(namespace, writer_epoch)` are dense `1..T`, so the next record's id is
computable and **the round never asks the listing what to read**… **THE LISTING IS A HINT**, and
demoting it is the point of this loop. It used to be the source of truth… **Under arithmetic intake
such an omission is a NON-EVENT: the exact GET finds the record anyway**». За подсказкой остались
ровно две роли: genesis не-фолдившегося namespace и witness-множество, делающее отсутствие
ожидаемого-следующего разрешимым (`:1849-1855`); пропущенный id выше ожидаемого HOLD'ит namespace с
неподвинутым курсором, а не проглатывается. Переход эпох идёт только через consumed `EpochSeal`
(INV-2) по обратной цепочке `prev_epoch_seal` (`:1858-1863`), то есть эпоху, которую LIST потерял
целиком, всё равно обходят. Механика в коде: `chainLinkFor`/`makeEpochSealTxn`
(`Pool/CasRefLedger.cpp:123,136-150`), приземление зафиксировано в
`docs/superpowers/cas/BACKLOG/ref-protocol.md:17`.

Формальная сторона тоже уже адъюдицирована и НЕ требует переспора:
`docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md:233-266` — модель сознательно не
переписывалась, `_sab_holeylist` оставлен КРАСНЫМ как исторический свидетель pre-v9, а регрессионную
пару образуют `CaRefTableSnapshotLogCore_sab_scanistruth` (RED, свойство не вакуумно) и
`CaRefDeltaIntakeCore_v9_hintomission` (GREEN, под v9 та же пропажа безвредна). Там же прямо
написано: красный `_sab_holeylist` «says nothing about v9 and must not be read as a v9 failure».
Смежное settled-решение по доверию к LIST: `docs/superpowers/cas/2026-08-03-list-trust-verdict.md`
(консолидирован в `docs/en/antalya/cas/architecture/*`, см.
`docs/superpowers/cas/consolidation-2026-08/COVERAGE-MATRIX.md:272`).

**Что действительно остаётся — это прозаический дефект, и он в отгружаемом исходнике.**
`src/Storages/MergeTree/DataPartsExchange.cpp:1383-1387` до сих пор утверждает: «`_sab_holeylist`
shows that with every confirm rule intact and one incomplete listing page permitted,
`ConfirmedRelinkNeverDangles` still breaks (BACKLOG `{#list-as-journal-dataloss-2026-07-25}`). A
confirmed relink is therefore NOT proven dangle-free». Утверждение описывает pre-v9 состояние, а
якорь BACKLOG мёртв. Читатель кода получит из него неверный вывод о текущих гарантиях. Правка:
переписать абзац под арифметический интейк (сослаться на `CasGc.cpp:1844-1863` и на пару
scanistruth/hintomission), а не удалять — оговорка про «`yes` означает только, что источник прямо
сейчас держит именно этот манифест» остаётся верной. Тот же устаревший тезис продублирован в
`docs/superpowers/cas/2031-triage.md:1172,1186` — исторический снимок, править не нужно.

### NV-5 (подтверждено, P2) {#nv-5}

**Форма подтверждена дословно.** `Parts/PartFolderAccess.cpp:646` —
`bool CachedPartFolderAccess::dropRefIfMatches(...) noexcept`; `try` закрывается на `:702`, а
`eraseView(key)` стоит на `:705`, ЗА пределами `catch (...)`. Тот же дефект в соседней функции:
`dropRefBestEffort(...) noexcept` (`:627`) — `try/catch` до `:640`, `eraseView(key)` на `:644`.

**Ответ на вопрос «allocation-free ли `recordDecision`/`CacheBase::remove` для уже присутствующего
ключа»: он не тот, который решает дело — аллокация происходит РАНЬШЕ и БЕЗУСЛОВНО.**
`eraseView` первой же строкой делает `const String cache_key = key.cacheKey();`
(`Parts/PartFolderAccess.cpp:308`), а `cacheKey()` — это
`return ns.string() + '\0' + ref;` (`Parts/PartFolderAccess.h:34`), то есть две конкатенации
`std::string` = как минимум одна куча-аллокация на КАЖДЫЙ вызов, независимо от состояния кэша,
независимо от `explain_enabled` и независимо от того, включена ли retention (`view_cache` может быть
`nullptr` — ключ всё равно построен). Под `MEMORY_LIMIT_EXCEEDED`/`bad_alloc` это бросок из
`noexcept`-функции → `std::terminate`. Именно на пути отката при нехватке памяти, как и написано в
находке.

Дополнительно (второстепенно, но подтверждает): `recordDecision` при `params.explain_enabled`
(`Parts/PartFolderAccess.h:239`, по умолчанию `false`) делает `explain_map[cache_key]`
(`.cpp:728`) — `operator[]` на `unordered_map` ВСТАВЛЯЕТ отсутствующий ключ, то есть аллоцирует узел
и копию строки; для инвалидации ключ вполне может быть новым. `CacheBase::remove(key)`
(`src/Common/CacheBase.h:281-285`) — `lock_guard` + `cache_policy->remove`, аллокаций не видно.

**Фикс.** Либо занести `eraseView(key)` внутрь существующего `try` (сохранив «инвалидируем в любом
случае» через второй `catch`), либо обернуть его собственным
`try { eraseView(key); } catch (...) { tryLogCurrentException(...); }`. Второй вариант точнее
сохраняет намерение комментария на `:703-704`. То же — для `dropRefBestEffort`.

### NV-6 (частично (по существу дубликат уже отслеживаемых CAS-118 и CAS-055), P3 (чтение) / P2 (hardlink-путь)) {#nv-6}

**Множитель — пофайловый.** Каждая read-side точка входа метаданных сама парсит путь и берёт view:
`ContentAddressedMetadataStorage.cpp:1846` (`getStorageObjects`), `:1883`
(`tryGetStorageObjects`), `:1923`, `:1966` — все они начинаются с `parsePartFilePath(path)` +
`route(*p)` и требуют непустого `p->file`, то есть вызываются ИМЕННО на файл, а не на часть. Никакого
короткого замыкания через уже загруженные в память `checksums`/список колонок
`IMergeTreeDataPart` тут нет: `MergeTreeReaderStream` открывает каждый файл колонки через
`readFile`/`getFileSize`, и каждый такой вызов заходит в metadata storage.

**Но стоимость одного повтора — не запрос к объектному хранилищу.** `getView`
(`Parts/PartFolderAccess.cpp:160-191`) сначала делает `resolve(key, freshness)` →
`store->resolveRef(..., allow_stale = (freshness == CachedForLoad))`, что для read-пути является
чисто in-memory поиском в `RefTableState`, затем при совпадении `manifestId()` отдаёт удержанный view
(`:176-190`, `CASPartFolderViewHits`) — `readManifestShared` не вызывается. То есть backend-цена
остаётся O(частей), а не O(частей × файлов).

Это ровно то, что уже записано в BACKLOG как CAS-118
(`BACKLOG/performance.md:484-500`): «One `DiskObjectStorage::readFile` on a blob-backed part file
walks that chain twice… and a third time whenever the caller sized the file first through
`getFileSize`… **This is CPU and lock traffic only, not requests**: `resolveRef` is a pure in-memory
lookup… and a warm `CachedForLoad` view hit returns without touching `readManifestShared`». Остаток —
взятие `pointer_mutex` пула + `state_mutex` ref-таблицы + лок view-кэша + одна `String`-аллокация
`cacheKey()` на каждый проход, умноженные на число файлов широкой части.

**Где множитель действительно дорогой — не на чтении, а на `createHardLink`** (CAS-055,
`BACKLOG/performance.md:304-320`): там `ForceFresh` при отгружаемом дефолте
`part_folder_validate = always` никогда не отдаёт удержанный view (короткое замыкание на
`Parts/PartFolderAccess.cpp:197` гейтится на `validate.mode != Always`), поэтому каждый вызов
доходит до `buildView` → `readManifestShared` с ОБЯЗАТЕЛЬНЫМ `HEAD`. `FREEZE`/clone или
`ALTER … UPDATE` хардлинкают каждый неизменённый файл части — один `HEAD` на файл. Фикс уже
существует в соседней функции (`unlinkFile` мемоизирует доказательство per `(transaction, ref)` в
`force_fresh_validated_refs`, `ContentAddressedTransaction.cpp:1595-1603`).

Итог по M7: множитель — «на открытие файла», как и опасалось ревью, но последствие на read-пути —
CPU/локи, а не запросы; отдельный пункт заводить не нужно, оба сегмента уже в BACKLOG.

### NV-7 (не подтвердилось, P3) {#nv-7}

Цикл действительно не может сматчиться, когда `deduplication_logs` — последний компонент:
`Parts/PartPathParser.cpp:358` — `for (size_t i = 1; i + 1 < p.size(); ++i)`. Но следствие,
которого опасается находка («misclassified as a file with that name»), наблюдаемых последствий не
имеет, потому что оба пути дают ОДИН И ТОТ ЖЕ результат:
- зарезервированная ветка при `i == p.size()-1` дала бы
  `table_uuid = joinTableId(p, 0, i)` и `tail = joinTableId(p, i, p.size())` (`:360-365`);
- fallback даёт `table_uuid = joinTableId(p, 0, p.size() - 1)` и `tail = p.back()` (`:369-372`).
При `i == p.size()-1` это тождественно: тот же префикс и тот же единственный хвостовой компонент
`"deduplication_logs"`. Для Atomic-раскладки вопрос вообще не встаёт — там `tail` есть всё после
uuid одной строкой (`:339-347`).

**Функция ДЕЙСТВИТЕЛЬНО вызывается с голым путём-каталогом, и это штатный путь.**
`ContentAddressedTransaction.cpp:1149-1163` — ветка `removeDirectory` «Table-level SUBDIRECTORY
(deduplication_logs/): remove every verbatim file under it» использует `tf->tail + "/"` как префикс
для `listNamespaceFiles`; при `tail == "deduplication_logs"` префикс получается ровно
`"deduplication_logs/"` — правильный. Аналогично `ContentAddressedMetadataStorage.cpp:1471-1477`
классифицирует форму как `DirShape::TableSubdir`.

Остаток чисто стилистический: цикл нужен ТОЛЬКО для непоследнего вхождения (путь вида
`data/db/tbl/deduplication_logs/<file>`, где fallback свернул бы подкаталог в table id) — там он
обязателен и работает. Дефекта нет.

### NV-8 (частично, P3) {#nv-8}

Ревью искало `LOG_*` рядом с `complete_error` и не нашло — но лог стоит уровнем ниже, внутри самой
конструкции исключения:
- `Backend/CasRequestControl.cpp:237-242` — `makeCasWriteRetryLaterExceptionPtr(why)` ПЕРВОЙ строкой
  вызывает `logCasWriteRetryLater(why)` и только потом делает `std::make_exception_ptr`;
- `:232-236` — `throwCasWriteRetryLater(why)` делает то же самое перед броском;
- `:223-228` — сам `logCasWriteRetryLater`: `LOG_WARNING(log, "CAS write could not be committed
  ({}); retrying later", why)`.
Сайт заклинивания ref-lane использует ровно эту фабрику: `Pool/CasRefLedger.cpp:3926-3934` —
`ProfileEvents::increment(ProfileEvents::CASRefAppendWedged)` и следом
`complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format("CAS ref-log append
for namespace '{}' txn {}-{} is UNCERTAIN ({}) — the append lane is wedged…")))`. Соседний арм
(отказ ДО отправки запроса) — `:3905-3917`, тот же механизм плюс отдельный счётчик
`CASRefAppendPreAttemptRefused`. Путь исчерпания бюджета conditional-write
(`Backend/CasRequestControl.cpp:461`, `:828`) возвращает `Unresolved`, который его вызывающие
переводят в тот же `throwCasWriteRetryLater`.

Второй, независимый уровень: исключение, вышедшее из фоновой задачи, всегда логируется
универсальным перехватом исполнителя — `src/Storages/MergeTree/MergeTreeBackgroundExecutor.cpp:201`
и `:208`, `tryLogCurrentException(log, "Exception while executing background task {…}")`. Отдельно,
detached-публикатор снапшотов свои падения тоже логирует сам
(`Pool/CasRefLedger.cpp:4004-4007`, `:4023`).

**Что действительно частично.** `logCasWriteRetryLater` использует
`LogSeriesLimiter(getLogger("CasWriteRetryLater"), /*allowed_count=*/1, /*interval_s=*/30)`, а
`LogSeriesLimiter` ключуется ТОЛЬКО по имени логгера (это зафиксировано и в комментарии на
`:218-222`, и как известная ловушка). Значит на весь класс «CAS write could not be committed»
печатается одна строка в 30 секунд: под штормом конкретное заклинивание фоновой мутации может
не напечататься вовсе, и авторитетным сигналом остаются счётчики
(`CASRefAppendWedged`, `CASRefAppendPreAttemptRefused`) плюс общая строка исполнителя. То есть
заявленная находка («no adjacent `LOG_*`») не подтверждается, а реальный остаток — агрегирующий
троттл, и он документирован в самом коде.

### NV-9 (подтверждено (дубликат B4 в части последствия), P1) {#nv-9}

`CasRefLedger::drainRefLanesForShutdown` (`Pool/CasRefLedger.cpp:1847-1905`) выставляет
`shutting_down`, снимает список runtime'ов и ждёт ровно двух вещей:
`while (!(rt->pending.empty() && !rt->leader_active))` под `ref_queue_mutex` (`:1874-1886`) и затем
проверяет `lane_state` на `Writing`/`Wedged` (`:1895-1902`). Ни `pending_snapshot_publishes`, ни
`publish_settle_cv` в функции не упоминаются вовсе.

Ожидание, которого тут не хватает, в дереве УЖЕ НАПИСАНО дважды:
- `quiesceRefTablesForRemount`, `:1705-1706` —
  `rt->publish_settle_cv.wait(slock, [&] { return rt->pending_snapshot_publishes.load(...) == 0; });`
- `dropNamespaceImpl`, `:5102-5107` — тот же цикл с комментарием «No background publisher may carry
  the old runtime across the later catalog deletion».
Плюс тестовый хелпер `:4100`.

Публикатор запускается как `ThreadFromGlobalPool(...).detach()` (`:3998-4013`) и держит
`auto owner = pin_owner()` — сильную ссылку на `Pool`. Поэтому это НЕ use-after-free, а именно
отложенный `~Pool`: ровно то, что B4 называет «detached threads that defer `~Pool`», и что делает
возможным исполнение прощальной долговечной записи и эмита события уже после гашения object storage
и обнуления `Context` (B3). B4 адъюдицирован как подтверждённый P1 «до релиза»
(`opus-review-triage.md:49`), так что NV-9 не добавляет нового дефекта — он ОТВЕЧАЕТ на вопрос B4:
нет, ничто не ограничивает публикатора относительно `shutdown()`, и фикс — добавить в
`drainRefLanesForShutdown` тот же `publish_settle_cv`-цикл, но с ограничением по общему
`wait_budget_ms` (в отличие от двух существующих безлимитных ожиданий).

Второе утверждение находки («тогда останется только диагностическая нить
`reportImpossibleInterference`») подтвердить не удалось и оно не требуется для вывода: даже если
других detached-нитей нет, публикатор один уже делает предпосылку B4 верной.

### NV-10 (не подтвердилось (подтверждаю RESOLVED, как и записало само ревью), —) {#nv-10}

Механика на HEAD жива дословно: `src/Common/HTTPConnectionPool.cpp:879` —
`if (!connection.connected() || connection.mustReconnect() || !connection.isCompleted() ||
connection.buffered())` — соединение не переиспользуется; `isCompleted()` объявлен на `:642`,
`flushRequest()` переопределён на `:470` с вызовом `Session::flushRequest()` на `:496`. Это
пред-существующая машинерия, к CAS отношения не имеющая; она же покрывает два ранних выхода
`DataPartsExchange` с недочитанным телом. Ничего добавлять не нужно; пункт записан ради полноты.

### NV-11 (не разрешено (по существу — вне кода), P3) {#nv-11}

Из кода проверяемо ровно две вещи, и обе подтверждаются:
1. Явное поколение с рабочим forward-гейтом живо: `Formats/CasFormat.h:62` —
   `constexpr uint32_t G_BUILD = 10;` (на момент обзора было 9, то есть с тех пор прошёл ещё один
   бамп — счёт `9 pre-release bumps` устарел на единицу), плюс именованные backward-полы
   (`:78-95`) и отказ `UNKNOWN_FORMAT_VERSION` для значений выше `G_BUILD` (`:143-147`).
2. Политика при каждом бампе — та же: «the pool must be recreated; there is **no migration path in
   the pre-release format**» (`Formats/CasFormat.h:31`, `:78`), и каждое поколение подписано как
   `recreate-only` (`:81`, `:85`, `:89`, `:92`, `:95`).

Чем закрывается по-настоящему: решением владельца продукта, зафиксированным в
`docs/en/antalya/cas/` (страница совместимости/поддержки) и в changelog-записи релиза, — «с какой
версии формат становится стабильным и что мы обещаем при бампе после релиза». Кодовое изменение,
которое из этого решения следует (если решат поддержать миграцию) — это отдельная работа, а не
проверка. Смежное: `opus-review-triage.md` m26 (на ветке нет ни одной changelog-строки про CAS,
P2, до релиза) — именно там это решение и должно проявиться текстом.

### NV-12 (не подтвердилось (подтверждаю RESOLVED, как и записало само ревью), —) {#nv-12}

Пути выбора реплики в `src/Storages/StorageReplicatedMergeTree.cpp` перемешиваются равномерно
`thread_local_rng`: `:2316`, `:3577`, `:5217`, `:5397`, `:11887` — все пять
`std::shuffle(replicas.begin(), replicas.end(), thread_local_rng)`. Вывод ревью («цикл relink'а
недостижим при ≥3 репликах и достижим при 2») из этой формы следует напрямую и на HEAD не изменился.
Случай двух реплик — это свойство самой топологии, а не дефект выбора.

### NV-13 (не подтвердилось, P3) {#nv-13}

**Обоснование в интерфейсе точное, но описание пути устарело.** `IMetadataStorage.h:373-377`
говорит про «per-ref sidecar»; в коде отдельного мутабельного sidecar-пути уже НЕТ:
`ContentAddressedTransaction.cpp:872-878` — «The former mutable-per-part-file branch
(uuid.txt/metadata_version.txt/txn_version.txt staging directly into a separate mutable payload) is
**DELETED** here — these three names fall through to the ordinary content path below like any other
tree file… a standalone write on an already-committed part repoints». Это стоит поправить в
комментарии интерфейса (мелочь, но именно на неё смотрит следующий читатель).

**Атомарность.** `supportsAtomicFileWrites()` у CAS честно `true`
(`ContentAddressedMetadataStorage.h:261`), поэтому `VersionMetadataOnDisk::storeInfoToDataPartStorage`
берёт однократную ветку записи без `tmp`+`replaceFile`
(`src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp:326-336`). Публикация файла на
закоммиченной части идёт через `publishStaging` → `repointRef` → `promote(allow_repoint=true)`,
который кладёт в ОДНУ запись ref-лога три op'а — снятие старой committed-привязки, переход
precommit→committed и `SetPublishedAt` (`Pool/CasPartWriteTxn.cpp:895-925`); внутрирекордного
промежуточного состояния не наблюдаемо («`applyRefLogTxn`'s whole-record scratch apply makes the
composition a sound refinement», `:908-911`). Полусостояния файла не существует.

**Видимость.** После долговечного append `promoteBuild` делает `eraseView(key)`
(`Parts/PartFolderAccess.cpp:358`), так что следующее чтение резолвится заново; `getView`
дополнительно валидирует удержанный view по `manifestId()` против свежего resolve
(`:176-181`).

**Конкурентность (то, ради чего вопрос задавался).** Запись `txn_version.txt` по одной части
сериализована на уровне MergeTree: `VersionMetadataOnDisk::storeInfo` берёт
`std::lock_guard lock_storing_version{persisted_info_mutex}` (`VersionMetadataOnDisk.cpp:280-283`) и
дополнительно проверяет `expected_storing_version != new_info.storing_version → TOO_OLD_VERSION`
(`:224-231`). Двух одновременных писателей этого файла по одной части нет.

Единственный остаток — общий с NV-1: если бы такой второй писатель (по ЛЮБОМУ файлу той же
закоммиченной части) появился, carry-forward repoint потерял бы его изменение молча. То есть
гарантия MVCC здесь держится на MergeTree-мьютексе, а не на CAS-предусловии, — что ещё один довод
за дешёвую страховку из NV-1.

### NV-14 (частично подтверждено, P3) {#nv-14}

**Итератор действительно отдаёт прокси.** `DatabaseOrdinary::loadTableFromMetadataLazy` создаёт
`std::make_shared<StorageTableProxy>(table_id, std::move(get_nested), std::move(columns))` и
`attachTable`'ит В КАТАЛОГ именно прокси (`src/Databases/DatabaseOrdinary.cpp:485-488`), поэтому
`database->getTablesIterator(...)->table()` возвращает `StorageTableProxy`, пока таблица не
материализована. Значит `dynamic_cast` к реальному движку в fan-out'ах промахивается.

**Доктрина хелпера их не оправдывает.** `unwrapTableProxy`'s комментарий
(`InterpreterSystemQuery.cpp:278-282`) говорит: «generic **query paths** already materialize on read
by design and must not go through this helper». Перечисленные сайты — не query paths, а
`SYSTEM`-глаголы, обходящие каталог; на них аргумент не распространяется.

**Сайты на HEAD (нумерация сместилась; их четыре, а не три):**
- `:1653` — `SYSTEM RESTART REPLICAS`: `if (dynamic_cast<const StorageReplicatedMergeTree *>(it->table().get()))`.
  Следствие: не материализованные lazy-реплики молча не попадают в список и не перезапускаются.
  Этого сайта в списке ревью не было — он найден дополнительно.
- `:1749` — `SYSTEM DROP REPLICA … FROM ZKPATH`, локальная защитная проверка:
  `if (auto * storage_replicated = dynamic_cast<StorageReplicatedMergeTree *>(iterator->table().get()))`
  и сравнение `getReplicaPath() == remote_replica_path`. **Самое существенное следствие:** lazy-таблица
  не проверяется, и пользователь может снести в ZK реплику живой локальной таблицы. Смягчение —
  комментарий на `:1743` сам называет проверку избыточной («This check is actually redundant, but it
  may prevent from some user mistakes»), то есть это деградация защиты в глубину, а не первичной
  гарантии.
- `:2248` — обход для обновления набора частей по диску: `dynamic_cast<MergeTreeData *>(it->table().get())`.
  Здесь итератор берётся с `skip_not_loaded=true` (`:2246`), а само действие — перестроение набора
  частей, которое материализованная позже таблица сделает сама; следствий нет.
- `:2747` — `SYSTEM LOAD/UNLOAD PRIMARY KEY` для всех таблиц:
  `if (auto * merge_tree = dynamic_cast<MergeTreeData *>(it->table().get()))`. Пропуск
  не материализованной таблицы безвреден: у неё нечего выгружать.
(Сайт `:2727` в исходном списке ревью — ложное срабатывание: он кастует `table`, полученный уже
через `unwrapTableProxy` на `:2725`.)

**Область.** Это не CAS: изменение пришло коммитом `3d35dfce282` (табличный прокси-unwrap, Tier B в
разделе split'а того же ревью — «unrelated to CAS; no reason to be on this branch»). Правильное
действие — либо развернуть прокси и в fan-out'ах (тогда `SYSTEM RESTART REPLICAS` и защита
`DROP REPLICA` перестают зависеть от `lazy_load_tables`, ценой материализации каждой таблицы, что
противоречит смыслу ленивой загрузки), либо честно сузить доктрину в комментарии хелпера, назвав
fan-out осознанным исключением и его последствие для `DROP REPLICA`. Решение принадлежит владельцу
upstream-поверхности, не CAS-ветке (см. правило «не менять upstream-поверхности без консультации»).
