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
| B6 | ⏳ | — | — | — | — |
| B7 | ⏳ | — | — | — | — |
| B8 | ⏳ | — | — | — | — |
| B9 | ⏳ | — | — | — | — |

## Major M1-M12 {#majors}

| ID | Статус на HEAD | Приоритет | До релиза? | Где отслеживается | Суть |
|----|----------------|-----------|------------|-------------------|------|
| M1 | ⏳ | — | — | — | — |
| M2 | ⏳ | — | — | — | — |
| M3 | ⏳ | — | — | — | — |
| M4 | ⏳ | — | — | — | — |
| M5 | ⏳ | — | — | — | — |
| M6 | ⏳ | — | — | — | — |
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
