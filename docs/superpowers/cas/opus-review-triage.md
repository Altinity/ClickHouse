# Триаж umbrella review (Opus) от 2026-08-05 {#opus-review-triage}

Источник: `docs/superpowers/cas/random/opus-review-20250805.md` — второй обзор того же диапазона
(`5e8eaeb4d7d` → `056488b47a0`), 15 полос ревью, вердикт «request changes». Отличается от
`fable-review-triage.md` (обзор того же дня другой полосой) прежде всего разделом blast radius —
что регрессирует у тех, кто CAS никогда не включит.

Каждый пункт перепроверен против HEAD ветки `cas-gc-rebuild` (2026-08-22). Где пункт покрывает ту же
землю, что уже разобранный пункт `fable-review-triage.md` или `2031-triage.md`, ставится
**дубликат** со ссылкой — заново не выводим.

Статусы: **исправлено** · **подтверждено** · **частично** · **не подтвердилось** · **by-design** ·
**дубликат**. Приоритет: P1 (до релиза) · P2 · P3 · — .

## Blast radius вне CAS — Tier 1-3 {#blast-radius}

Tier 1 = подтверждённые регрессии (T1-T2), Tier 2 = глобальные изменения поведения (T3-T7),
Tier 3 = конфигурация и упаковка (T8-T12).

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| T1 | ⏳ | — | — | — | — |
| T2 | ⏳ | — | — | — | — |
| T3 | ⏳ | — | — | — | — |
| T4 | ⏳ | — | — | — | — |
| T5 | ⏳ | — | — | — | — |
| T6 | ⏳ | — | — | — | — |
| T7 | ⏳ | — | — | — | — |
| T8 | ⏳ | — | — | — | — |
| T9 | ⏳ | — | — | — | — |
| T10 | ⏳ | — | — | — | — |
| T11 | ⏳ | — | — | — | — |
| T12 | ⏳ | — | — | — | — |

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
| M7 | ⏳ | — | — | — | — |
| M8 | ⏳ | — | — | — | — |
| M9 | ⏳ | — | — | — | — |
| M10 | ⏳ | — | — | — | — |
| M11 | ⏳ | — | — | — | — |
| M12 | ⏳ | — | — | — | — |

## Minor m1-m31 {#minors}

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
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
| m13 | ⏳ | — | — | — | — |
| m14 | ⏳ | — | — | — | — |
| m15 | ⏳ | — | — | — | — |
| m16 | ⏳ | — | — | — | — |
| m17 | ⏳ | — | — | — | — |
| m18 | ⏳ | — | — | — | — |
| m19 | ⏳ | — | — | — | — |
| m20 | ⏳ | — | — | — | — |
| m21 | ⏳ | — | — | — | — |
| m22 | ⏳ | — | — | — | — |
| m23 | ⏳ | — | — | — | — |
| m24 | ⏳ | — | — | — | — |
| m25 | ⏳ | — | — | — | — |
| m26 | ⏳ | — | — | — | — |
| m27 | ⏳ | — | — | — | — |
| m28 | ⏳ | — | — | — | — |
| m29 | ⏳ | — | — | — | — |
| m30 | ⏳ | — | — | — | — |
| m31 | ⏳ | — | — | — | — |

## Needs verification NV-1…NV-14 {#needs-verification}

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| NV-1 | ⏳ | — | — | — | — |
| NV-2 | ⏳ | — | — | — | — |
| NV-3 | ⏳ | — | — | — | — |
| NV-4 | ⏳ | — | — | — | — |
| NV-5 | ⏳ | — | — | — | — |
| NV-6 | ⏳ | — | — | — | — |
| NV-7 | ⏳ | — | — | — | — |
| NV-8 | ⏳ | — | — | — | — |
| NV-9 | ⏳ | — | — | — | — |
| NV-10 | ⏳ | — | — | — | — |
| NV-11 | ⏳ | — | — | — | — |
| NV-12 | ⏳ | — | — | — | — |
| NV-13 | ⏳ | — | — | — | — |
| NV-14 | ⏳ | — | — | — | — |

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
