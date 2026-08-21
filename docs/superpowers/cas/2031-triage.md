# Триаж issue #2031 — 135 findings статического AI-аудита {#triage-2031}

Источник: https://github.com/Altinity/ClickHouse/issues/2031 (аудит от 2026-08-12 против
`cas-code-only-strip` @ `842f2b37b8f`, т.е. кода PR #2159). Полные тексты findings — gist
`alsugiliazova/6dce01834f93cdb7cdbb2fc70d1efc5f` (локальная копия: `tmp/2031/gist/`,
нарезка по ID: `tmp/2031/findings/`).

Каждый finding проверен отдельно против текущего HEAD ветки `cas-gc-rebuild` (2026-08-21+).
Приоритеты расставлены заново. Статусы: **подтверждено** (реальная проблема, есть на HEAD) ·
**исправлено** (было реально, уже починено — с указанием коммита/механизма) · **not-a-bug**
(утверждение фактически неверно / галлюцинация) · **by-design** (поведение намеренное, позиция
зафиксирована) · **частично** (часть утверждений верна) · **дубликат** (см. другой CAS-###).
Приоритет: P1 (до релиза) · P2 (после релиза, трекается) · P3 (низкий/косметика) · — (не нужен).

## Сводная таблица {#summary-table}

| ID | Статус | Приоритет | BACKLOG | До релиза? | Summary |
|----|--------|-----------|---------|------------|---------|
| CAS-001 | подтверждено (↗ #2212) | P1 | [{#issue-2212-shadow-namespace}](BACKLOG.md#issue-2212-shadow-namespace) | да | shadow/FREEZE namespace пул-глобальный; UNFREEZE одного сервера удаляет frozen-парты другого |
| CAS-002 | by-design | P3 | — | нет | Отсутствие probe/condemn-check в `adoptEvidence` — сознательный дизайн §4 (manifest-trust, коммит `8fe6331a431`); заявленное окно data-loss на HEAD закрыто на каждом call-site (relink publish-then-confirm, `republishRef` commit-before-release, MergeTree-пиннинг источника при hardlink), остаток — принятый D4 trade-off с fsck-backstop. |
| CAS-003 | частично | P3 | [{#gc-budgets-need-a-deadline}](BACKLOG.md#gc-budgets-need-a-deadline) | нет | Факты (нет wall-clock TTL, кража по дифференциальному наблюдению, deposed-лидер узнаёт о потере на round-commit CAS) верны, но это дизайн: разрушительные pre-CAS действия обоснованы только ранее опубликованным durable-состоянием + exact-token delete, а каталожные/ref-фазы явно ревалидируют lease перед каждым delete — утверждение «destructive phases are never revalidated» на HEAD ложно; остаток — только liveness (учтён в BACKLOG). |
| CAS-004 | частично | P2 | [gc-rebuild-lease-interlock] (docs/superpowers/cas/BACKLOG/gc.md:142, без {#}-якоря) | нет | Серверный `SYSTEM CAS GC REBUILD` на HEAD безопасен на живом диске (ничего не condemn-ит, GC-lease + барьеры), «противоположные read-only позы» двух входов — задокументированный дизайн; реально остался только зафиксированный в BACKLOG пробел: у офлайн `clickhouse-disks cas-gc-rebuild` нет mount-lease интерлока против живого сервера. |
| CAS-005 | частично | P3 | [{#codecs-and-protocol}](BACKLOG.md#codecs-and-protocol) | нет | Ядро находки закрыто рефакторингом TXN-ONE-PIPELINE (публикация ref только в `commit`, точный откат по `CommitOutcome`, диагностика вместо молчания); остались осознанные, задокументированные и отслеживаемые в BACKLOG остатки — отсутствие мультиреф-атомарности, невозврат repoint'а committed-ref и immediate-класс DDL-операций. |
| CAS-006 | частично | P3 | — | нет | Механизм подтверждён (пер-ref перенос без журнала, крэш посреди RENAME оставляет таблицу расщеплённой между namespace, авто-реконсилера нет), но потеря данных сильно преувеличена: перенос идемпотентно передрайвливается повторным RENAME, ничего физически не удаляется, «refs, добавленные во время обхода» блокируются эксклюзивной блокировкой таблицы, а сама ветка достижима только для non-Atomic (deprecated Ordinary) баз. |
| CAS-007 | подтверждено | P2 | [{#nested-srid-decommission}](BACKLOG.md#nested-srid-decommission) | нет | Вложенный `server_root_id` (`a/b`) принимается валидацией, а владение в decommission определяется префиксом пути, поэтому `SYSTEM CAS DROP POOL MEMBER` на `a` удаляет пространства имён и объекты живого участника `a/b`. |
| CAS-008 | by-design | — | [{#read-write} (BACKLOG/performance.md, пункт [ch128ctx])](BACKLOG.md#read-write} (BACKLOG/performance.md, пункт [ch128ctx]) | — | Позиция подтверждена на HEAD: хеш выбираем (`blob_hash` = cityhash128\|xxh3-128\|sha256, дефолт cityhash128, фиксируется при создании пула), повторного хеширования на чтении нет — осознанное решение, не баг. |
| CAS-009 | частично | P2 | [{#disk-error-audit-followups-2026-07-21}](BACKLOG.md#disk-error-audit-followups-2026-07-21) | нет | Механика описана верно (presence-only admit, ни один повторный аплоад/staged-body не пере-хэшируется), но digest-половина — by-design, а реальное окно порчи сводится к length-preserving расхождению локального scratch и уже отслеживаемой неатомарной записи emulated-бэкенда. |
| CAS-010 | частично | P2 | [{#empty-token-unconditional-write-guard}](BACKLOG.md#empty-token-unconditional-write-guard) | нет | Механизм реален — пустой `Token` в Native-режиме проходит все проверки и уходит на провод БЕЗ `If-Match`, но ни одного call site, где токен пуст по конструкции, на HEAD нет: это отсутствующий fail-closed guard, а не доказанный путь потери данных. |
| CAS-011 | частично | P3 | [{#ref-protocol-rev6} (пункт «[timeout-retry RFC residuals]», подпункт (c)); смежное — `BACKLOG/performance.md` «[codex-26] `casAppendObject`»](BACKLOG.md#ref-protocol-rev6} (пункт «[timeout-retry RFC residuals]», подпункт (c)); смежное — `BACKLOG/performance.md` «[codex-26] `casAppendObject`) | нет | Обход `CasRequestController` — факт и уже отслеживаемый residual, но заявление про обход fence ложно (fence-generation проверяется перед каждым durable PUT/DELETE и покрыто тестами); 100 попыток без сна — тормоз от live-lock, не политика повторов. |
| CAS-012 | частично | P3 | [{#bucket-requirements-lifecycle-worm-glacier}](BACKLOG/docs-and-cleanup.md#bucket-requirements-lifecycle-worm-glacier) | нет | Урегулированная позиция (by-design + docs) на HEAD держится, но док-половина закрыта только по versioning: lifecycle expiration, Object Lock/WORM и storage-class transitions в `docs/en/antalya/cas/` не описаны нигде, а Glacier-чтение падает сырым `S3Exception` без restore-and-retry и без классифицированной подсказки. |
| CAS-013 | частично | P3 | — | нет | Механика подтверждена (допуск алгоритма CAS-поднимает пул-глобальный `min_reader_generation` до `G_BUILD` без записи блоба), но заявленного вреда сегодня нет: обратный порог формата уже равен `G_BUILD`, поэтому старые сборки и так не читают пул — дефект латентный, на будущее окно совместимости. |
| CAS-014 | частично | P2 | [{#part-file-suffix-allowlist-memory}](BACKLOG/performance.md#part-file-suffix-allowlist-memory) | нет | Классификатор действительно закрытый allowlist и не знает `primary.cidx`, `.mrk4`/`.cmrk4` и файлов вторичных индексов — но это не corruption: есть cap 1 MiB и спилл в blob, реальная цена — буферизация всего файла в памяти и двойная запись. |
| CAS-015 | частично | P2 | [{#no-query-cancellation-checks}](BACKLOG/ref-protocol.md#no-query-cancellation-checks) | нет | Ожидания на single-flight/лидере/восстановлении действительно без дедлайна и без отмены запроса, но каждое из них стоит за ограниченным по времени I/O (бюджет `CasRequestController` 90 с/16 попыток, бюджет восстановления 120 с, потеря mount-fence), поэтому «вечного» зависания нет — остаётся неотменяемость (`KILL QUERY`/`max_execution_time`) и суммирование ограниченных операций в минуты. |
| CAS-016 | частично | P3 | [{#ref-protocol-rev6} (пункт «[timeout-retry RFC residuals]», подпункт (c), `BACKLOG/ref-protocol.md:20`)](BACKLOG.md#ref-protocol-rev6} (пункт «[timeout-retry RFC residuals]», подпункт (c), `BACKLOG/ref-protocol.md:20`) | нет | Оба факта верны буквально — `attempt_timeout_ms` действительно не уходит на провод, а чтение payload идёт обычным путём объектного хранилища, — но и то и другое задокументированный дизайн; «нет таймаута попытки, нет лимита попыток, нет классификации» ложно, реальный остаток — отсутствие startup-сверки бюджета с `request_timeout_ms` диска и уже отслеживаемые в бэклоге небюджетированные read/HEAD/LIST. |
| CAS-017 | частично | P3 | [{#lane-residuals-2031-cas-017}](BACKLOG/ref-protocol.md#lane-residuals-2031-cas-017) | нет | Порядок «защёлка до durable» и терминальность `Closed`/`Faulted` подтверждены, но «пустой catch глотает reopen», «нет выхода из `Removing`» и «постоянно нерабочая таблица» на HEAD ложны — есть fail-close-разрешение с закреплённым тестом, GC-реклейм `Removing` и полностью in-memory защёлка; реальный остаток — read-путь отвечает «нет ref» вместо retry-later в окне защёлки и одна ветка `Faulted` без автоматического remount. |
| CAS-018 | частично | P3 | [{#noexcept-allocation-hardening}](BACKLOG/ref-protocol.md#noexcept-allocation-hardening) | нет | Головной механизм (утечка лидерства в ref-очереди) уже закрыт единой точкой выхода и тестами; из шести якорей подтверждаются только теоретические аллокации в `noexcept`/деструкторах под лимитом памяти, а «renewal фенсит маунт» — прямо неверно. |
| CAS-019 | частично | P2 | [{#part-folder-single-flight-manifest-keying}](BACKLOG/ref-protocol.md#part-folder-single-flight-manifest-keying) | нет | Ключ single-flight действительно только `ns+ref` и post-wait проверки manifest id нет, но каждый выданный view внутренне консистентен (один манифест), сингл-флайт работает только на stale-терпимом `CachedForLoad`, так что последствие — сдвиг на один репойнт, а не смешение двух манифестов. |
| CAS-020 | подтверждено | P2 | [{#move-out-copies-envelope-bytes}](BACKLOG/formats-and-storage.md#move-out-copies-envelope-bytes) | нет | Механизм подтверждён — `getStorageObjects` теряет смещение payload и не имеет CA-гарда на стороне ИСТОЧНИКА, поэтому серверный copy-object (MOVE из CA / BACKUP на s3 того же хоста) копирует байты конверта; но «без ошибки» преувеличено: inline-файлы отдают ПУСТОЙ ключ, и операция целиком падает громко. |
| CAS-021 | частично | P3 | [{#cas-021-followups}](BACKLOG.md#cas-021-followups) | нет | Все шесть цитат про контроллер на HEAD текстуально верны, но каждое опасное следствие уже нейтрализовано; остаточный дефект — устаревшая in-process памятка condemn-маркера, чинить её пере-чтениями пользователь отказался (принятый остаток + переименование в наблюдаемости). |
| CAS-022 | частично | P2 | [{#orphan-sweep-absent-catalog-row-window}](BACKLOG/gc.md#orphan-sweep-absent-catalog-row-window) | нет | Ветка «нет строки в каталоге» в постраничном планировщике sweep-а действительно не проверяет ни watermark, ни coverage, ни §6-предпосылку, и её обоснование («строка каталога публикуется раньше любого объекта жизни») на HEAD неверно — тело манифеста пишется до создания строки; но окно узкое, а последствие громкое и не data-loss. |
| CAS-023 | частично | P3 | [{#consolidation-2026-08-findings} (пункт `[gc-enabled-false-silent]`)](BACKLOG.md#consolidation-2026-08-findings} (пункт `[gc-enabled-false-silent]`) | нет | Оба поведения на HEAD подтверждены как факты кода, но класс `DATA-LOSS` не подтверждается: `gc_enabled=false` документирован как отладочный режим и уже стоит в BACKLOG, vanished-пул no-op-успех — сознательный fail-close контракт; неверна лишь формулировка «every manual reclamation verb is refused» (FSCK и `GC STOP` работают). |
| CAS-024 | not-a-bug | P3 | — | нет | Конфигурация «два CAS-диска на одном пуле с одинаковым `server_root_id`» не доживает до записи: второй диск падает на mount-протоколе с `ABORTED` (live double-start), поэтому пути потери данных при `MOVE PARTITION TO DISK` нет. |
| CAS-025 | by-design | P3 | [{#gc-followups}](BACKLOG.md#gc-followups) | нет | Механика описана верно (rebuild стартует с пустых priors, свод edge-only, condemn-универсум сбрасывается, инкрементальный fold такие блобы больше не найдёт), но это осознанный fail-closed компромисс, уже зафиксированный в BACKLOG как «REBUILD R4 residual»: это удержание (retention), а не потеря, видимое как недренирующийся fsck `unaccounted`. |
| CAS-026 | by-design | P3 | — | нет | Идентичность relink — не «только `pool_uuid`»: это pool_uuid + server_root_id + namespace + ref + part + точный `ManifestRef`, доказываемый publish-then-confirm у источника; отсутствие probe блобов — сознательный §4 manifest-trust (`8fe6331a431`), а `check_consistency=false` — байт-в-байт то же, что и в upstream байтовом пути. |
| CAS-027 | by-design | P3 | [{#pool-trust-boundary-undocumented}](BACKLOG/docs-and-cleanup.md#pool-trust-boundary-undocumented) | нет | Код на HEAD ровно соответствует урегулированной позиции (никакой intra-pool аутентификации нет, bucket-credential = вся граница доверия); единственный реальный остаток — эта граница доверия не описана нигде в `docs/en/antalya/cas/`. |
| CAS-028 | by-design | P3 | `BACKLOG/operability-and-introspection.md` {#operability} — пункты **[B14]** (expedited/GDPR right-to-erasure delete) и **[B17]** (encryption-at-rest × content-addressing, «dedup scope per-encryption-key») | нет | Ключи блобов на HEAD действительно неподсолённые пул-глобальные хеши контента — это и есть суть CAS-дедупа (не пересматриваем); dedup-оракул через `system.cas_log` требует явного гранта (не доступен непривилегированному пользователю), а вот отсутствие crypto-shred-примитива подтверждается и в операторской документации не написано ни одной строкой. |
| CAS-029 | частично | P3 | [{#versioning-enabled-after-mount}](BACKLOG/formats-and-storage.md#versioning-enabled-after-mount) | нет | Центральное утверждение находки ложно: versioning-предусловие проверяется НЕ конфигурационной GCS-проверкой, а обязательным поведенческим mount-пробом (`created_delete_marker`), который отбивает любой versioned-бакет на AWS и любом S3-совместимом сторе; реально остались три узких остатка — fail-open GCS-проверки, включение versioning ПОСЛЕ монтирования (там `LOGICAL_ERROR` вместо нормальной ошибки, и только на пути тела блоба) и `skip_access_check`. |
| CAS-030 | частично | P3 | [{#skip-access-check-no-signal}](BACKLOG/formats-and-storage.md#skip-access-check-no-signal) | нет | Механика верна (probe целиком пропускается, decommission выставляет флаг жёстко), но «removes every bucket-configuration defense» преувеличено — single-attempt gate и residual-proof остаются, а реальный остаток — отсутствие ЛЮБОГО сигнала оператору о пропущенном probe и самопротиворечивое сообщение про versioning «has no override». |
| CAS-031 | подтверждено | P2 | [{#write-once-probe-misses-multipart}](BACKLOG/formats-and-storage.md#write-once-probe-misses-multipart) | нет | Верно: обе write-once CREATE-примитивы (streaming `putIfAbsentStream` и server-side `promoteStaged`) для больших тел переносят `If-None-Match` на `CompleteMultipartUpload`, а обе probe-проверки экзерсайзят только маленький single-operation путь — т.е. батарея сертифицирует не тот путь, по которому идёт основной объём блобов; но эксплуатируемо только на сторонних S3-совместимых хранилищах (AWS требование поддерживает, GCS отказывается громко). |
| CAS-032 | частично | P2 | [{#pool-exclusive-prefix-undocumented}](BACKLOG/docs-and-cleanup.md#pool-exclusive-prefix-undocumented) | нет | Форма кода подтверждена — идентичность пула нигде не привязана к endpoint/бакету, но вредное следствие требует операторской ошибки (запись в CRR-приёмник / двунаправленная репликация поверх префикса), которая нарушает уже подразумеваемое, но НЕ задокументированное требование «префикс принадлежит только CAS»; фикс = требование в docs + необязательная advisory-запись endpoint в mount-lease. |
| CAS-033 | by-design | P2 | [{#ckpt-damage-no-repair-path}](BACKLOG.md#ckpt-damage-no-repair-path) | нет | Ворота действительно пул-широкие и без границы — это осознанный fail-closed выбор («лучше не рекламировать, чем удалить лишнее»), позиция в коде подтверждается; но «нет сигнала оператору» — фактически неверно (WARNING с разбором причин, ProfileEvent, `suppressed` в phase-метриках, `pending_reclaim`/`wedged_namespace_count`), а гранулярность уже трекается в BACKLOG. |
| CAS-034 | частично | P2 | [{#janitor-page-hardcoded}](BACKLOG/gc.md#janitor-page-hardcoded) | нет | Формы кода подтверждены (5000 ref-объектов/раунд, одна страница janitor'а на 1000 ключей), но следствие — отложенная утилизация и рост латентности стирания, а не потеря/порча данных; SLA стирания закрыт позицией автора, арифметика «бюджет vs скорость создания» реальна и уже отслеживается в BACKLOG. |
| CAS-035 | подтверждено | P2 | [{#fold-edge-run-memory}](BACKLOG/gc.md#fold-edge-run-memory) | нет | Подтверждено по всем пунктам, причём пик памяти хуже, чем описано: раунд GC делает полное перечисление `cas/ns/stream/` с удержанием всех ключей в памяти и материализует ВЕСЬ новый edge-run в одной строке в памяти (при дефолтном `gc_shards=1` — целиком по пулу); класс уже полностью отслежен в BACKLOG как O(pool)-per-round. |
| CAS-036 | частично | P3 | [{#control-object-read-precap-materialization}](BACKLOG.md#control-object-read-precap-materialization) | нет | Из трёх заявленных механик на HEAD жива одна с половиной: тело control-объекта действительно материализуется целиком ДО проверки `object_cap`, а дедупликация ключей JSON остаётся Θ(k²); «read buffer по размеру, объявленному атакующим» и «resize по объявленному размеру zstd-фрейма до проверки» — фактически неверны, а сам «атакующий» по урегулированной модели доверия = держатель bucket-креденшла, т.е. полностью доверенная сторона, и всякий отказ громкий (`MEMORY_LIMIT_EXCEEDED`/`CORRUPTED_DATA`), без тихой порчи. |
| CAS-037 | частично | P3 | [{#numeric-parse-and-window-wrap}](BACKLOG.md#numeric-parse-and-window-wrap) | нет | Обёртка `readIntText` без проверки переполнения — реальный код-шейп, и два настоящих остатка есть (`std::stoull` принимает `-1` в трёх разборах GC-ключей, из которых один даёт `max_gen + 1 == 0`; `location.offset + location.length` может завернуться и схлопнуть окно чтения в EOF), но центральный тезис «wrapping defeats every decoder range gate» логически несостоятелен, а `-1` в телах, underflow `blob_header_len - 1`, аллокация по length-полю и «Poco multiplication» — неверны либо недостижимы. |
| CAS-038 | частично | P3 | [{#decoder-optional-field-residuals} (новая секция; внутри {#outcome-log-oc-not-required} и {#gc-state-encode-no-line-cap}); ранее существующий {#seal-decode-remaining-fields} — устарел (закрыт)](BACKLOG.md#decoder-optional-field-residuals} (новая секция; внутри {#outcome-log-oc-not-required} и {#gc-state-encode-no-line-cap}); ранее существующий {#seal-decode-remaining-fields} — устарел (закрыт) | нет | Из семи подпретензий пять факту на HEAD не соответствуют или имеют безопасное значение по умолчанию; реально остались только необязательный `oc` в журнале исходов GC и отсутствие проверки line cap на стороне записи `gc/state` — и то и другое косметика. |
| CAS-039 | частично | P3 | [{#gc-shards-no-upper-bound} (новая секция; внутри {#gc-shards-config-override-silent}); родственный существующий {#sec4-decoder-size-bounds}](BACKLOG.md#gc-shards-no-upper-bound} (новая секция; внутри {#gc-shards-config-override-silent}); родственный существующий {#sec4-decoder-size-bounds) | нет | Форма описана верно (верхней границы у `gc_shards` нет нигде, а локальное значение из XML молча замещается пуловым), но последствия — громкий fail-closed отказ аллокации и отсутствие warning'а, а не порча данных; отдельно неверно утверждение об отсутствии сравнения — расхождение durable-пары ловится и бросает. |
| CAS-040 | подтверждено | P1 | [{#manifest-entry-path-newline-banner}](BACKLOG.md#manifest-entry-path-newline-banner) | да | Механика находки реальна и достижима обычным DDL (проекция с `\n` в имени), но следствие описано неверно: коммита нечитаемой части нет (INSERT падает fail-closed, данные не теряются) — зато осиротевший манифест навсегда заклинивает каждый раунд GC во всём пуле. |
| CAS-041 | частично | P2 | [{#manifest-digest-by-reencode}](BACKLOG.md#manifest-digest-by-reencode) | нет | Механика (digest = канонический re-encode) на HEAD ровно как описано, но заявленное следствие `CORRUPTED_DATA` на чужом поле сегодня недостижимо (версионный гейт срабатывает раньше и громче); реальный остаток — политика `Tolerant` для этого формата мертва плюс измеренные 27-63% времени декода и две лишние копии payload. |
| CAS-042 | by-design | P2 | [{#operability} (B180 / format-freeze), {#cas-format-version-floor}](BACKLOG.md#operability} (B180 / format-freeze), {#cas-format-version-floor) | нет | Форма кода описана верно (одна глобальная генерация как min-reader, `changePoints` не читается на декоде), но это осознанная pre-release политика recreate-only; следствия про «тихое стирание полей» и про `Roster` недостижимы. |
| CAS-043 | частично | P3 | [{#relink-fallback-unknown-format-version}](BACKLOG.md#relink-fallback-unknown-format-version) | нет | Узость catch подтверждена (`CORRUPTED_DATA` only, а гейт версии и критический ключ дают `UNKNOWN_FORMAT_VERSION`), но перекос генераций в одном пуле сегодня невозможен: relink предлагается только внутри одного смонтированного пула, а mount держит точный гейт генерации — остаётся однострочное упрочнение. |
| CAS-044 | подтверждено | P2 | [{#manifest-inline-budget-no-spill}](BACKLOG.md#manifest-inline-budget-no-spill) | нет | Агрегатный лимит 16 MiB inline-данных на манифест действительно проверяется только в stageManifest и не имеет пути переклассификации в blob — INSERT/мерж падает громко и воспроизводимо; достижимость шире, чем описано в находке. |
| CAS-045 | подтверждено | P2 | [{#part-folder-cache-weight-always-256}](BACKLOG.md#part-folder-cache-weight-always-256) | нет | Вес записи part-folder кэша всегда равен 256 байт, потому что `Resolved::manifest_size` оба производителя жёстко пишут нулём — байтовый бюджет `part_folder_cache_bytes` и порог oversized-bypass неработоспособны. |
| CAS-046 | подтверждено | P2 | [{#disk-error-audit-followups-2026-07-21} + {#scale-findings}](BACKLOG.md#disk-error-audit-followups-2026-07-21} + {#scale-findings) | нет | Все части описания кода верны на HEAD (локальный scratch = полные байты части, без резервирования/учёта/квоты/лимита, удаляется только в конце транзакции, стартовой уборки нет), но это уже дважды затреканный DESIRABLE-класс, а не новый дефект: отказы громкие (ENOSPC fail-loud), тихой порчи нет. |
| CAS-047 | by-design | P3 | [{#writepath-candidates-post-stage1}](BACKLOG.md#writepath-candidates-post-stage1) | нет | Форма кода описана верно (один процессный пул, 16 потоков, queue_size == max_threads, enqueue блокирующий), но привязанное следствие ложно: блокировка на enqueue — это штатный backpressure, а не сериализация и не дедлок; размер — серверная настройка, тюнинг уже затрекан. |
| CAS-048 | частично | P3 | [{#covering-part-publish-under-datapartslock}](BACKLOG.md#covering-part-publish-under-datapartslock) | нет | Форма подтверждена — публикация пустой покрывающей части действительно идёт под `DataPartsLock`, но путь редкий (только DROP/REPLACE PARTITION), работа маленькая и ограниченная, отказ громкий, а на обычном object-storage-диске тот же lock уже удерживается на время записи части. |
| CAS-049 | частично | P2 | [{#lifecycle-verbs-wait-out-uncancellable-scans} (частично покрыто ранее в {#fsck-scale-timeout})](BACKLOG.md#lifecycle-verbs-wait-out-uncancellable-scans} (частично покрыто ранее в {#fsck-scale-timeout}) | нет | Сериализация подтверждена и в большинстве мест сознательна; реальный остаток — отсутствие кооперативной отмены (GC-раунд и FSCK нельзя прервать, SQL FSCK не убивается KILL QUERY и не передаёт deadline), при этом «shutdown ждёт FSCK» неверно, а «unbounded scans» неточно — раунд ограничен work-бюджетами. |
| CAS-050 | частично | P2 | [{#gc-scheduler-stop-join-race}](BACKLOG.md#gc-scheduler-stop-join-race) | нет | Гонка данных на объектах потоков в `CasGcScheduler::stop` подтверждена (join вне `mutex`, достижимо через `SYSTEM CAS DROP POOL MEMBER` параллельно с GC STOP/shutdown), но вторая половина находки — «joinable-but-dead планировщик, который сообщает, что работает» — практически не воспроизводится и вредом не является. |
| CAS-051 | частично | P2 | [{#snapshot-publish-fanout-unbounded}](BACKLOG.md#snapshot-publish-fanout-unbounded) | нет | Утечка счётчика `pending_snapshot_publishes` при провале dispatch — исправлена ещё `829ad698ef6`, поэтому «вечное ожидание» в `quiesceRefTablesForRemount`/`dropNamespaceImpl` недостижимо; подтверждается только вторая половина: fan-out фоновых публикаций не ограничен пулом-широко (по одному потоку на namespace). |
| CAS-052 | not-a-bug | — | — | — | Форма кода описана верно, но следствие недостижимо: `Pool` всегда живёт под `shared_ptr`, все синхронные вызовы приходят от владельцев `PoolPtr`, а detached-поток пинит пул копией `self`. |
| CAS-053 | частично | P3 | [{#ref-table-cache-budget-admission-only}](BACKLOG.md#ref-table-cache-budget-admission-only) | нет | Единственная точка вызова и незащищённое вычитание подтверждаются, но «раз на namespace и больше никогда» неточно (пасс идёт на каждое холодное admission), а последствия — только лишний recovery-I/O и мягкий, а не жёсткий, потолок памяти; корректность не страдает. |
| CAS-054 | частично | P3 | [{#debug-body-counter-assert-on-replay}](BACKLOG.md#debug-body-counter-assert-on-replay) | нет | Главное обвинение (O(R) пере-кодирование в `admits`) устарело — закрыто ещё в июле; снапшот раз в 256 транзакций — это by-design чекпойнт в фоновом потоке; реально остался только debug/sanitizer-ассерт, возвращающий O(K·N) на replay. |
| CAS-055 | подтверждено | P2 | [{#hardlink-per-file-forcefresh-head}](BACKLOG.md#hardlink-per-file-forcefresh-head) | нет | Подтверждено: ветка carry-forward в `createHardLink` делает ForceFresh-resolve на каждый файл, а при дефолтном `part_folder_validate = always` это обязательный `HEAD` манифеста на файл; но «полная пересборка view» преувеличена (декод берётся из кэша), а фикс — мемоизация уровня транзакции, уже существующая в `unlinkFile`. |
| CAS-056 | частично | P2 | [{#standalone-write-scratch-manifest-cost}](BACKLOG.md#standalone-write-scratch-manifest-cost) | нет | форма кода реальна — standalone-запись в закоммиченную часть действительно платит второй (черновой) manifest-PUT и по одному adopt-событию на перенесённый blob-лист, но «два ПОЛНЫХ manifest-энкода» и «внутри retry-замыкания CAS» — неточности; корректность не страдает. |
| CAS-057 | not-a-bug | P3 | [{#tmp-replacefile-on-committed-part}](BACKLOG.md#tmp-replacefile-on-committed-part) | нет | бросок `LOGICAL_ERROR` на не-стейдженный `moveFile`/`replaceFile` подтверждён и сохраняет ранее принятую позицию (fail-loud-заглушка без живого вызывающего), а «свежая улика» ложная: `DeleteBitmapFileOps::writeBitmapToStorage` не имеет ни одного продакшн-вызова. |
| CAS-058 | подтверждено | P1 | [{#issue-2173-freezeremote-gap}](BACKLOG.md#issue-2173-freezeremote-gap) | да | `freezeRemote` действительно единственный из трёх clone-путей без CAS-транзакции, кросс-дисковый `ATTACH PARTITION FROM` в CAS падает на первой же части — это уже подтверждённый и воспроизведённый на HEAD issue #2173 с запланированным пред-релизным фиксом; неверны только «REPLACE PARTITION FROM» в триггере и намёк на тихое «partial state» (отказ громкий, tmp-часть подчищается). |
| CAS-059 | by-design | P3 | [{#encrypted-over-cas-missing-gate} (новая запись; фича-часть — {#operability} `[B17]`)](BACKLOG.md#encrypted-over-cas-missing-gate} (новая запись; фича-часть — {#operability} `[B17]`) | нет | Все описания кода на HEAD верны (`DiskEncrypted` берёт любой делегат, не переопределяет `isContentAddressed`/`supportsAtomicFileWrites`, `use_fake_transaction` по умолчанию true), но CAS+шифрование — settled out-of-scope позиция (Filimonov), а связка отваливается громким `NOT_IMPLEMENTED` на первой же записи части; остаток — только отсутствующий fail-fast гейт в конфиге, P3. |
| CAS-060 | by-design | P3 | [{#operability} ([B17]) + новая секция {#encrypted-wrapper-hides-content-addressed}](BACKLOG.md#operability} ([B17]) + новая секция {#encrypted-wrapper-hides-content-addressed) | нет | Форма кода верна (случайный IV на каждую перезапись + CAS хеширует то, что ему дали → дедупа нет вовсе), но CAS×шифрование — принятая out-of-scope позиция; тихой порчи нет, а сама связка вообще не проведена (`DiskEncrypted` не пробрасывает `isContentAddressed`). |
| CAS-061 | частично | P2 | BACKLOG.md {#damaged-object-repair} + BACKLOG/gc.md {#ckpt-damage-no-repair-path} + новая секция {#pool-meta-bootstrap-blocks-dr-tools} | нет | Ядро подтверждено (единственный rebuild-верб — `gc/state`; каталог/`_ckpt` падают fail-closed без пути восстановления; все CA-инструменты открываются через `_pool_meta`), но это уже затрекано как {#damaged-object-repair}/{#ckpt-damage-no-repair-path}, часть про mount lease неверна (есть `cas-drop-member`), а отсутствие migration-тулинга — сознательное pre-release решение. |
| CAS-062 | частично | P3 | [{#fsck-meta-body-counters-unrendered} (новый); дубликат по таймауту/скоупу — {#lifecycle-verbs-wait-out-uncancellable-scans}](BACKLOG.md#fsck-meta-body-counters-unrendered} (новый); дубликат по таймауту/скоупу — {#lifecycle-verbs-wait-out-uncancellable-scans) | нет | SQL-путь FSCK действительно counts-only без дедлайна и скоупа, но это уже отслеживается как CAS-049; «нет пути ремонта нигде» неверно, а исключение meta/body-счётчиков из `clean()` — by-design; реальный остаток — эти два счётчика не рендерятся ни на одной поверхности. |
| CAS-063 | частично | P3 | [{#owner-only-slot-invisible-in-mounts} (новый)](BACKLOG.md#owner-only-slot-invisible-in-mounts} (новый) | нет | Порядок «дропы namespace → снятие слота» и фильтр `/mount` в `listMounts` подтверждаются, но «повторный запуск не может починить» — фактически неверно: resume-путь есть в коде и закреплён тестами; остаётся только невидимость owner-only слота в `system.content_addressed_mounts`. |
| CAS-064 | by-design | P3 | [{#format-battery-three-classes-unregistered}](BACKLOG.md#format-battery-three-classes-unregistered) | нет | Отсутствие фаззера и property-based тестов подтверждено фактически, но это settled-позиция (доверяем S3, декодеры fail-closed); реальный остаток — три живых формат-класса не зарегистрированы в общей battery (P3, дрейф-риск, а не дыра в покрытии). |
| CAS-065 | частично | P2 | [review #14] в docs/superpowers/cas/BACKLOG/testing-and-ci.md#tests + [GATE #1: Azure] / [GCS production-grade follow-ups] в BACKLOG/formats-and-storage.md#backends | нет | Центральное утверждение ложно — нативный `If-None-Match`/`If-Match` путь гоняется в шести CI-полосах CAS-over-S3 (RustFS, `Mode::Native`) и на каждом writable-открытии пула проходит fail-closed capability-battery; реально не хватает только повторяемой полосы для GCS-generation-диалекта и native-строки в contract-suite, и это уже заведено. |
| CAS-066 | by-design | P2 | [{#local-backend}](BACKLOG.md#local-backend) | нет | Форма кода подтверждена (режим выбирается по типу хранилища, ручки нет, лог на INFO) — это осознанное проектное решение с записанным обоснованием; риск «два сервера на одном локальном пути» уже отслеживается как doc-долг B26/B135, а под-претензия про read-only монтирование последствий не имеет. |
| CAS-067 | частично | P3 | [{#emu-token-state-clock-skew-leak}](BACKLOG.md#emu-token-state-clock-skew-leak) | нет | Первая половина (грубая гранулярность mtime даёт валидный устаревший токен) закрыта mtime-quantum-дисамбигуатором ещё 2026-07-18 и покрыта тестом; вторая (перекос часов ломает истечение token-state) реальна, но это только утечка памяти в emulated-режиме без последствий для корректности — заведён новый MINOR-пункт. |
| CAS-068 | by-design | P3 | [{#putifabsent-swallowed-attempt-cause}](BACKLOG.md#putifabsent-swallowed-attempt-cause) | нет | Форма кода реальна и осознанна (расхождение с четырьмя «сиблингами» зафиксировано в коде и в коммите `4f4f93c6bc6`), но заявленный триггер (`promoteStaged`/`resurrect` → `NOT_IMPLEMENTED`) в этой полосе недостижим, а последствие — громкий fail-closed wedge, а не порча; остаётся только потеря диагностики. |
| CAS-069 | частично | P3 | [{#rebuild-gcstate-decode-reason-unreported}](BACKLOG.md#rebuild-gcstate-decode-reason-unreported) | нет | Пустые catch существуют и причину действительно теряют, но «неотличимо от порчи → полный rebuild → вечные орфаны по CAS-025» не выводится: rebuild запускается только руками (`SYSTEM CAS GC REBUILD`), а на undecodable-ветке он не сбрасывает holds, а находит seal перечислением и отказывается с `CORRUPTED_DATA`; про `stoull`-катчи утверждение о недосчёте `max_gen` неверно. |
| CAS-070 | частично | P2 | [{#remount-running-latched-before-spawn}](BACKLOG.md#remount-running-latched-before-spawn) | нет | Из трёх заявленных механизмов реален только один — `remount_running` латчится до создания потока, поэтому упавший спавн навсегда выключает само-ремоунт; «lost wakeup» и «нет обработчика в теле потока» на HEAD не подтверждаются. |
| CAS-071 | by-design | P3 | — | нет | Позиция прежнего вердикта (CAS-090: by-design, latent) на HEAD в силе — `mount_keeper` меняется только под `Pool::remount_mutex` и его единственный «конкурент» отсечён конфиг-гейтом в коде; заявленный разрыв fence/deadline против `mayMutate()` фактически неверен, остаётся один косметический остаток (`pool_uuid` публикуется вне `pointer_mutex`). |
| CAS-072 | частично | P3 | [{#precommit-add-single-slot-guard}](BACKLOG.md#precommit-add-single-slot-guard) | нет | Форма кода реальна (один слот precommit, перезаписывается без проверки), но второй `precommitAdd` на одном `PartWriteTxn` не достижим ни на одном рабочем пути — остаётся латентный инвариант без исполняемой защиты. |
| CAS-073 | by-design | P3 | [{#cas-021-followups}](BACKLOG.md#cas-021-followups) | нет | Все три формы верны, но следствие изобретено: маркер по определению не является авторитетом удаления — удаляет exact-token delete по токену из retired-строки, а resurrect ротирует incarnation-tag, так что маркер «прошлой инкарнации» ничего удалить не разрешает. |
| CAS-074 | ⏳ | — | — | — | — |
| CAS-075 | ⏳ | — | — | — | — |
| CAS-076 | ⏳ | — | — | — | — |
| CAS-077 | ⏳ | — | — | — | — |
| CAS-078 | подтверждено | P3 | [{#janitor-cursor-rewind-on-list-error}](BACKLOG.md#janitor-cursor-rewind-on-list-error) | нет | Сброс курсора уборщика namespace на любой ошибке LIST реален и запинен тестом, но это только задержка реклейма (громкая, посчитанная fsck), а не потеря данных. |
| CAS-079 | подтверждено | P2 | [{#ref-cleanup-whole-catalog-token-stillness}](BACKLOG.md#ref-cleanup-whole-catalog-token-stillness) | нет | Ревалидация ref-cleanup в GC действительно требует неподвижности токена всего пул-глобального каталога и при отказе выходит из всей фазы — живой остаток того самого класса, который для ref-writer уже убран коммитом 684161dcc03. |
| CAS-080 | ⏳ | — | — | — | — |
| CAS-081 | ⏳ | — | — | — | — |
| CAS-082 | ⏳ | — | — | — | — |
| CAS-083 | ⏳ | — | — | — | — |
| CAS-084 | ⏳ | — | — | — | — |
| CAS-085 | ⏳ | — | — | — | — |
| CAS-086 | ⏳ | — | — | — | — |
| CAS-087 | ⏳ | — | — | — | — |
| CAS-088 | ⏳ | — | — | — | — |
| CAS-089 | ⏳ | — | — | — | — |
| CAS-090 | ⏳ | — | — | — | — |
| CAS-091 | ⏳ | — | — | — | — |
| CAS-092 | ⏳ | — | — | — | — |
| CAS-093 | ⏳ | — | — | — | — |
| CAS-094 | ⏳ | — | — | — | — |
| CAS-095 | ⏳ | — | — | — | — |
| CAS-096 | ⏳ | — | — | — | — |
| CAS-097 | ⏳ | — | — | — | — |
| CAS-098 | ⏳ | — | — | — | — |
| CAS-099 | ⏳ | — | — | — | — |
| CAS-100 | ⏳ | — | — | — | — |
| CAS-101 | ⏳ | — | — | — | — |
| CAS-102 | ⏳ | — | — | — | — |
| CAS-103 | ⏳ | — | — | — | — |
| CAS-104 | ⏳ | — | — | — | — |
| CAS-105 | ⏳ | — | — | — | — |
| CAS-106 | ⏳ | — | — | — | — |
| CAS-107 | ⏳ | — | — | — | — |
| CAS-108 | ⏳ | — | — | — | — |
| CAS-109 | ⏳ | — | — | — | — |
| CAS-110 | ⏳ | — | — | — | — |
| CAS-111 | ⏳ | — | — | — | — |
| CAS-112 | ⏳ | — | — | — | — |
| CAS-113 | ⏳ | — | — | — | — |
| CAS-114 | ⏳ | — | — | — | — |
| CAS-115 | ⏳ | — | — | — | — |
| CAS-116 | ⏳ | — | — | — | — |
| CAS-117 | ⏳ | — | — | — | — |
| CAS-118 | ⏳ | — | — | — | — |
| CAS-119 | ⏳ | — | — | — | — |
| CAS-120 | ⏳ | — | — | — | — |
| CAS-121 | ⏳ | — | — | — | — |
| CAS-122 | ⏳ | — | — | — | — |
| CAS-123 | ⏳ | — | — | — | — |
| CAS-124 | ⏳ | — | — | — | — |
| CAS-125 | ⏳ | — | — | — | — |
| CAS-126 | ⏳ | — | — | — | — |
| CAS-127 | ⏳ | — | — | — | — |
| CAS-128 | ⏳ | — | — | — | — |
| CAS-129 | ⏳ | — | — | — | — |
| CAS-130 | ⏳ | — | — | — | — |
| CAS-131 | ⏳ | — | — | — | — |
| CAS-132 | ⏳ | — | — | — | — |
| CAS-133 | ⏳ | — | — | — | — |
| CAS-134 | ⏳ | — | — | — | — |
| CAS-135 | ⏳ | — | — | — | — |

---

# Детали по findings {#details}

## CAS-001 — shadow/FREEZE namespace пул-глобальный (подтверждено, P1) {#cas-001}

**Вердикт: подтверждено; уже выделено в #2212 и стоит в предрелизном списке.**

Ядро утверждения верно и было независимо подтверждено при адьюдикации
https://github.com/Altinity/ClickHouse/issues/2212 (2026-08-20): `shadowNamespace`
(`ContentAddressedMetadataStorage.cpp:1281`) строит namespace из literal
`shadow/<backup>/...`-пути **без** `serverPrefix()`, в отличие от `liveNamespace`. Комментарий на
месте называет это намеренным («backups are read by any replica»), но следствие — два сервера с
разными `server_root_id` на одном пуле являются несинхронизированными писателями одной shadow
ref-таблицы, и `UNFREEZE` любого из них удаляет frozen-парты обоих — принято как data-loss-класс.
Фикс (префикс `server_root_id` + две точки перечисления `"shadow/"`: `:1281`, `:1513`, `:1700` +
тест изоляции двух рутов) записан: BACKLOG `{#issue-2212-shadow-namespace}`, предрелизный список
`final-checks-todo.md` пункт 2.

Побочные утверждения detail-файла:
- «`DROP TABLE` оставляет shadow-refs, пиннящие байты навсегда» — **by-design**: это семантика
  апстримного `FREEZE` (бэкап переживает DROP таблицы и на локальном диске; освобождение — через
  `SYSTEM UNFREEZE`). Не дефект CAS.
- «отсутствие префикса отключает watermark floor / orphan-manifest sweep для этих namespace» —
  пересекается с CAS-022 (sweep и namespace без catalog-строки); разобрано там; на вердикт CAS-001
  не влияет — фикс #2212 переводит shadow-namespace в обычную per-root форму.

Статус в issue (`↗ split-out (#2212)`) корректен.

## CAS-004 — Серверный `SYSTEM CAS GC REBUILD` на HEAD безопасен на живом диске (ничего не condemn-ит, GC-lease + барьеры), «противоположные read-only позы» двух входов — задокументированный дизайн; реально остался только зафиксированный в BACKLOG пробел: у офлайн `clickhouse-disks cas-gc-rebuild` нет mount-lease интерлока против живого сервера. (частично, P2) {#cas-004}

Разбор по каждому утверждению находки, против кода на HEAD (ветка `cas-gc-rebuild`; аудированный снапшот `842f2b37b8f` в этом репозитории отсутствует — `git cat-file -t` → «Not a valid object name», так что старые якоря `CA/Gc/CasGc.cpp:2725` и `:2968-2971` проверялись по симоволам).

1) «Два входа требуют противоположных read-only поз» — ПОДТВЕРЖДАЕТСЯ КАК ФАКТ, но это BY-DESIGN, а не противоречие.
   - Серверный вход: `ContentAddressedMetadataStorage::runGcRebuildNow` начинается с `checkNotReadOnly("GC rebuild")` (src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:650) — ребилд выполняется внутри живого маунта сервера, который и так держит mount-lease.
   - Офлайн-инструмент: `programs/disks/CommandCaGcRebuild.cpp:54-58` требует `isReadOnly()` и объясняет зачем (строки 22-28): «this tool must never claim the live server's mount (a second live mounter racing the real GC's lease/writes is exactly the split-brain class the protocol is designed to prevent)». Запись `gc/state` при read-only открытии там же названа «a deliberate, explicit, operator-invoked exception». То есть «read-only does not gate writes» — правда, но это документированное исключение для одного CAS-плана записей GC, а не дыра.

2) «Nothing examines mount leases, mount slots or writer epochs; the only exclusion taken is the lease» — НА HEAD НЕВЕРНО в такой формулировке. Исключения и проверки у ребилда:
   - GC-lease одного лидера: `acquireOrRenewLease(state, state_token, /*allow_steal=*/false)` с отказом «another GC leader holds the lease» (Gc/CasGc.cpp:4010-4014) — исключает конкурирующие GC-раунды/ребилды.
   - На сервере весь ребилд сериализован под `gc_scheduler_mutex` с двойным `checkOpAdmitted(CasOpClass::Admin)` (lock-then-gate, ContentAddressedMetadataStorage.cpp:657-669; закрыто коммитом `4baddb748cc` 2026-07-23, до этого `runGcRebuildNow` вообще не брал лок — см. комментарий в src/Disks/tests/gtest_cas_forget.cpp:525).
   - Барьер каталога с проверкой авторитета: `drainCompletedRemoving` + `throwCasWriteRetryLater("CAS GC rebuild lost authority before the catalog settled")` (Gc/CasGc.cpp:4020-4025).
   - Mount-слоты ребилд ТАКИ трогает: `computeHeartbeatFloor(...)` в конце (Gc/CasGc.cpp:4342-4350) — «fence out any dead mounts as part of the disaster-recovery pass». Живые маунты по протоколу наблюдений НЕ фенсятся (CasServerRoot.h:446-481).

3) «Mount census result discarded» — ВВОДИТ В ЗАБЛУЖДЕНИЕ. Возвращаемые счётчики `HeartbeatFloor` действительно не используются, но это явно мотивировано в коде: «the returned classification counts are not needed for the round mint — graduation paces on rounds» (Gc/CasGc.cpp:4342-4343). Побочный эффект (фенс мёртвых маунтов) выполняется; никакого «отброшенного» результата, который должен был бы блокировать ребилд при живых писателях, там нет и не задумано. Отдельный read-only «census» `probeNonTerminalMountSlots` существует, но его потребитель — decommission-путь (Pool/CasPool.cpp:429), не ребилд.

4) «A rebuild is accepted on a live writable disk with inserts in flight» — ПРАВДА для серверного verb (`SYSTEM CAS GC REBUILD [FORCE] <disk>` → InterpreterSystemQuery.cpp:1033-1037, 2547-2563, за правом `SYSTEM_CAS_GC_REBUILD`), но заявленный класс INTEGRITY на HEAD не подтверждается:
   - Ребилд НИЧЕГО не condemn-ит и не удаляет: «The fold is EDGE-ONLY here: a rebuild condemns nothing» (Gc/CasGc.cpp:4112-4115) и большой блок «A REBUILD CONDEMNS NOTHING (spec §7)» (Gc/CasGc.cpp:4304-4321) — старый LIST-and-condemn хвост удалён коммитом `0cc71cece03` (2026-07-29) именно как вектор потери данных (r5-finding-4). Ошибка при конкурентных писателях смещена в сторону retention, не loss (см. также BACKLOG/gc.md:54 «[REBUILD R4 residual]»).
   - Холды прежнего seal едут verbatim (Gc/CasGc.cpp:4249-4264), нечитаемый prior seal — отказ `CORRUPTED_DATA` даже под FORCE (Gc/CasGc.cpp:3899-3929).
   - Вселенная ребилда — один замороженный catalog cut после завершённого hot LIST (Gc/CasGc.cpp:4026-4039); ссылки, закоммиченные после cut, лежат выше восстановленного фронтира и складываются следующим регулярным раундом — та же модель конкурентности, что у обычного раунда, который всегда работает при живых писателях.
   - Unowned-alive манифесты (trimmed-but-live билды) включаются с over-protect (Gc/CasGc.cpp:4273-4301).

5) Что РЕАЛЬНО осталось (и это уже записано): у офлайн `cas-gc-rebuild` единственная защита от живого сервера — дисциплина оператора «открой диск read-only»; свежий mount-lease живого сервера ребилд не останавливает. Это дословно BACKLOG-пункт `[gc-rebuild-lease-interlock]` — HARD, docs/superpowers/cas/BACKLOG/gc.md:142 («Real safety gap in a destructive tool»), добавлен в BACKLOG-триаж `f08734d17df` (2026-08-04) и вынесен в топ-3 раздела GC в docs/superpowers/cas/BACKLOG.md:27. Плюс известный смежный нюанс: над LOCAL-пулом эмуляция условных операций пер-процессная (ContentAddressedMetadataStorage.cpp:694-698), так что офлайн-инструмент рядом с живым сервером над одним локальным пулом дополнительно теряет и token-семантику.

Итог: ядро находки (нет writer/mount-интерлока у офлайн-инструмента) — подтверждено, но уже отслеживается в BACKLOG; остальная часть (серверный verb «принимает ребилд на живом диске» как INTEGRITY-проблема, «census discarded», «противоположные позы как баг») — на HEAD либо by-design и задокументировано, либо опровергается кодом (ребилд ничего не удаляет, эксклюзий несколько, а не одна). Формулировка one-liner'а «evidence strengthened — contradicts the partial's premise» кодом HEAD не подкрепляется: с момента снапшота ребилд стал строго менее опасным (`0cc71cece03`, `4baddb748cc`). Приоритет P2: закрыть `[gc-rebuild-lease-interlock]` (интерлок по свежести mount-lease в `rebuildBaseline` или в самом инструменте) стоит, но до релиза это не блокер — инструмент ручной, дисциплина задокументирована в самом инструменте, а серверный путь безопасен.

## CAS-003 — Факты (нет wall-clock TTL, кража по дифференциальному наблюдению, deposed-лидер узнаёт о потере на round-commit CAS) верны, но это дизайн: разрушительные pre-CAS действия обоснованы только ранее опубликованным durable-состоянием + exact-token delete, а каталожные/ref-фазы явно ревалидируют lease перед каждым delete — утверждение «destructive phases are never revalidated» на HEAD ложно; остаток — только liveness (учтён в BACKLOG). (частично, P3) {#cas-003}

Файлы на HEAD (`cas-gc-rebuild`, пути после переезда): `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp` (далее CasGc.cpp), `.../Gc/CasGcScheduler.cpp` (далее Scheduler.cpp).

(a) TTL/heartbeat — подтверждено фактически, но by-design. Wall-clock TTL у GC-lease действительно нет: `acquireOrRenewLease` (CasGc.cpp:4507-4618) решает по «дифференциальному наблюдению» — steal только если И кортеж lease `(owner, seq)`, И пара heartbeat `(owner, hb_seq)` заморожены между ДВУМЯ наблюдениями цикла претендента, разнесёнными на ≥ interval (CasGc.cpp:4573-4598; комментарий 4581-4590 прямо формулирует окно как двухтактовое). Это осознанный дизайн «без доверия к часам»: эффективный TTL ≈ 2 интервала планировщика заморозки обоих сигналов. Heartbeat пульсирует отдельный поток `heartbeatLoop` каждые `hb_interval = max(50ms, interval/4)` независимо от прогресса раунда (Scheduler.cpp:46-48, 355-388: «Advance the advisory heartbeat independently of round progress»), плюс пульс сразу при захвате lease, ДО долгого fold (`on_lease_acquired`, CasGc.cpp:518-527; Scheduler.cpp:106-119). `pulseHeartbeat` действительно отбрасывает результат `casPut` (CasGc.cpp:4491-4505) — но это задокументировано как advisory: потерянный пульс повторяется через hb_interval (Scheduler.cpp:115-117), а худший исход серии потерянных пульсов — лишняя кража, которая безопасна (см. (c)).

(b) Кража — подтверждено фактически, by-design (восстановление lease у мёртвого лидера обязано существовать). Уточнения против текста finding: (1) ручные раунды НИКОГДА не крадут — `allow_steal=false` для `SYSTEM ... GC RUN` и для rebuild (Scheduler.cpp:246-249; CasGc.cpp:4002-4014), закреплено коммитом `74d67b85021` («cas: manual GC rounds never steal a lease», 2026-07-13); adjudication #2211 (BACKLOG `{#issue-2211-gc-run-follower-noop}`) подтверждает эту позицию. (2) Смена hb-owner ПЕРЕвзводит окно (CasGc.cpp:4566-4572), т.е. живой новый лидер не украдётся из-за пульсов зомби. (3) Триггер finding «hb-записи падают при живых чтениях gc/state» реален, но исход — безопасная кража + fence, не потеря данных. Реальные остатки: (i) окно меряется тактами ПРЕТЕНДЕНТА (`gc_interval_sec` его диска) — при рассогласованных интервалах узлов быстрый претендент может украсть lease у живого медленного лидера (churn/liveness, не потеря); (ii) `rebuildBaseline` держит lease через one-shot `Gc` со свежим `gc_id` без hb-потока и без renew на протяжении всего скана (ContentAddressedMetadataStorage.cpp:675-682: «one-shot Gc instance is fine here»; CasGc.cpp:4002-4014), так что долгий rebuild может быть обкраден циклом соседа и абортируется на своём финальном CAS (CasGc.cpp:4377) — fail-closed, отказ, не два деструктора.

(c) «Destructive phases are never revalidated» — ЛОЖНО на HEAD, по двум независимым линиям. Во-первых, там, где ревалидации нет, она НЕ нужна по построению: «every destructive PRE-CAS action below is justified by PREVIOUSLY PUBLISHED durable state only (delete_pending entries), so replay under a fresh attempt is idempotent» (CasGc.cpp:522-527); единственный сайт удаления контента — exact-token `deleteExact` по записям, опубликованным ПРЕДЫДУЩИМ закоммиченным раундом, «justified by durable state and safe at any leader staleness» (CasGc.cpp:767-802); аргумент INV_NO_LOSS для exact-token удалений stale-лидера — CasGc.cpp:107-116 (ровно ради него отказались от spare-clear, см. историю §5); retention-prune специально защищает поколения родительского seal для случая проигравшего лидера (CasGc.cpp:1059-1069, «a losing leader must not destroy what the winning leader's already-adopted seal still points at»). Во-вторых, где идемпотентности недостаточно, ревалидация ЕСТЬ: ref-cleanup перед КАЖДЫМ `deleteExact` перечитывает каталог (token, row, life) И `gc/state` и останавливается при `lease.owner/seq != adopted` («GC fence moved», CasGc.cpp:3404-3461); pre-fold drain несёт `check_fence` по `(gc_id, lease.seq)` (CasGc.cpp:4640-4658). Наконец, весь раунд публикуется ОДНИМ CAS по `gc/state` со старым токеном (CasGc.cpp:1076-1079) — украденный lease ⇒ `ABORTED`, раунд deposed-лидера не имеет опубликованного эффекта. Утверждение finding «deposed-лидер выполняет весь redelete-батч и узнаёт на round-commit» верно буквально, но этот батч безопасен при любой staleness (двойное конкурентное исполнение exact-token удалений идемпотентно: NotFound/TokenMismatch толерируются, CasGc.cpp:802-828, 1193, 1243).

BACKLOG/история: тема «раунд, переживший окно наблюдения, фенсится» уже записана — BACKLOG.md `{#gc-budgets-need-a-deadline}` («a round holds the GC lease, and a round that outruns the lease TTL gets fenced — the wedge class already fixed once in P3.1»; правильная форма — wall-clock deadline на раунд + курсоры); rebuild-смежное — `BACKLOG/gc.md` `[gc-rebuild-lease-interlock]` (это про mount-lease офлайн-тула, отдельный вопрос). Прежний CAS-032 (зомби-пульс глушит hb нового лидера) закрыт: сейчас hb-liveness сознательно НЕ сравнивается с `current.lease.owner`, движение пары под ЛЮБЫМ owner читается как «жив» (CasGc.cpp:4563-4574) — ровно контр-мера против сценария CAS-032.

Итог: безопасностной ошибки (data loss от двух конкурентных GC-акторов) нет — конкурентное окно возможно by-design и безопасно (exact-token + prior-published-state + commit-CAS fence + явные lease-речеки в каталожных фазах). Остаток — liveness/churn (кража у живого медленного лидера при рассогласованных интервалах; не-возобновляемый lease на длинном rebuild ⇒ аборт), покрыт `{#gc-budgets-need-a-deadline}`; P3, не блокер релиза.

## CAS-006 — Механизм подтверждён (пер-ref перенос без журнала, крэш посреди RENAME оставляет таблицу расщеплённой между namespace, авто-реконсилера нет), но потеря данных сильно преувеличена: перенос идемпотентно передрайвливается повторным RENAME, ничего физически не удаляется, «refs, добавленные во время обхода» блокируются эксклюзивной блокировкой таблицы, а сама ветка достижима только для non-Atomic (deprecated Ordinary) баз. (частично, P3) {#cas-006}

Код на HEAD (ветка `cas-gc-rebuild`, коммит `684161dcc03`) найден по символам; старый якорь `CA/ContentAddressedTransaction.cpp:846-874` соответствует `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1225-1413` (`moveDirectory`), а `Parts/PartFolderAccess.cpp:419-431` — `PartFolderAccess.cpp:506-534` (`republishRef`).

1. Механизм подтверждён. Табличная ветка `moveDirectory` (оба конца — табличные директории): цикл `for (const auto & [ref, _] : ... listRefs(from_ns)) republishRef({from_ns, ref}, {to_ns, ref})` (`ContentAddressedTransaction.cpp:1258-1259`), затем копирование namespace-файлов (`:1265-1274`), затем терминальный `dropNamespace(from_ns)` (`:1275`). Снимок `listRefs` берётся один раз; журнала/реконсилера нет — комментарий это прямо признаёт: "There is no in-call compensation; true atomicity would need a durable move-journal (deliberately out of scope ...)" (`:1247-1249`). Крэш посреди цикла действительно оставляет таблицу «SPLIT across the two namespaces».

2. Но перенос спроектирован пере-драйвливаемым и без физической потери. `republishRef` сначала публикует манифест в приёмнике и только потом дропает источник (`PartFolderAccess.cpp:531-532`); при повторном заходе уже перенесённый ref обрабатывается идемпотентно: отсутствующий источник — no-op (`:511-513`), уже закоммиченный приёмник с равным содержимым — завершение переноса (`:520-528`), с различным — fail-loud `ABORTED` (`:523-526`). Блобы/деревья content-addressed и не трогаются; после крэша refs лежат либо в старом, либо в новом namespace, GC работает от refs, а не от каталога, так что ничего не выметается. Восстановление = повторить тот же `RENAME` (комментарий `:1243-1247`), при частичном отказе с исключением пишется громкий `LOG_ERROR` с обоими namespace (`:1279-1284`). Эта re-drivable-семантика + логирование добавлены коммитом `18451879788` (2026-06-20, "CA: harden write-path latent correctness (B123/B124/B126)") — т.е. после исходного аудита утверждение «no reconciler» частично устарело: ручной реконсилер (повторный RENAME) есть, автоматического — нет. До ручного передрайва таблица после рестарта видит только refs, оставшиеся в исходном namespace, т.е. часть партов временно «пропадает» из таблицы — это реальный residual, но восстановимый.

3. «Refs, добавленные во время обхода, дропаются» — на практике недостижимо. Единственный вызыватель табличной формы — `MergeTreeData::rename` (`src/Storages/MergeTree/MergeTreeData.cpp:4084`), который вызывается из `DatabaseOnDisk::renameTable` строго под `table->lockExclusively(...)` (`src/Databases/DatabaseOnDisk.cpp:468`, вызов `table->rename` на `:502`). Вставки/мержи/фетчи держат shared-lock на таблицу и не могут добавлять refs в `from_ns` во время обхода. Форма «дропнуть весь namespace после снапшота» в коде есть, но окно закрыто блокировкой.

4. Достижимость самой ветки узкая. `RENAME TABLE` в Atomic-базах данные не двигает вообще — `DatabaseAtomic::renameTable` вызывает только `renameInMemory` (`src/Databases/DatabaseAtomic.cpp:379`), UUID и путь `store/<u3>/<uuid>` не меняются. Табличная форма `parseTableUuid` для «оба конца — таблицы» срабатывает для Atomic-пути только на голой табличной директории (`PartPathParser.cpp:290-292`), но перемещений таких директорий Atomic не делает; реально ветка достижима через non-Atomic layout `data/<db>/<table>` (`PartPathParser.cpp:294-300`), т.е. deprecated Ordinary-базу (`allow_deprecated_database_ordinary`) поверх CAS-диска. Утверждение находки «RENAME TABLE across table UUIDs» неточно: UUID при RENAME никогда не меняется, меняется путь.

5. «Ordinary part removal takes the same path» — верно, но это не data-loss. Переименование в `delete_tmp_` идёт через part-dir-ветку и `republishRef` закоммиченного ref (`ContentAddressedTransaction.cpp:1407`); крэш между publish и drop оставляет ОБА ref (лишний `delete_tmp_*` подчищается штатной очисткой), потерь нет. Расточительность этого класса уже отслеживается: BACKLOG `docs/superpowers/cas/BACKLOG/gc.md:42` `[PART-REMOVAL-REPOINT]` (≈22% PUT-класса писателя) — но это про wasted work, не про потерю.

6. BACKLOG/история. Прямого пункта «move-journal / реконсилер для cross-namespace RENAME» в BACKLOG нет. Ближайший — `docs/superpowers/cas/BACKLOG/formats-and-storage.md:71` «[TXN-ONE-PIPELINE follow-up] committed-ref DDL overlay» (HARD, отложен): именно он фиксирует, что DDL-операции над закоммиченными refs (DROP/MOVE/RENAME TABLE через `removeDirectory`/`republishRef`/`dropNamespace`) остаются классом durable-at-call-time, не overlay-deferred, и описывает будущий `pending_ref_ops`-механизм; якоря `{#...}` у пункта нет. Крэш-консистентность RENAME он закрыл бы лишь частично (атомарность внутри транзакции, не durable-журнал между рестартами).

Итог: ядро находки (не-атомарный пер-ref перенос, снапшот один раз, терминальный `dropNamespace`, нет автоматического реконсилера) — правда и осознанный, задокументированный в коде компромисс. Преувеличения: потеря refs, добавленных во время обхода (блокируется эксклюзивной блокировкой), и «any part removal» как data-loss (это waste). Реальный остаток: после крэша посреди `RENAME` в Ordinary-базе таблица до ручного повторного RENAME видит не все парты, и никто кроме оператора это не чинит. С учётом того, что достижимость требует deprecated Ordinary DB поверх CAS-диска, а CAS pre-release — P3; при желании закрыть класс целиком — исполнить отложенный `[TXN-ONE-PIPELINE follow-up]` + durable move-journal (в коде помечен как deliberately out of scope).

## CAS-005 — Ядро находки закрыто рефакторингом TXN-ONE-PIPELINE (публикация ref только в `commit`, точный откат по `CommitOutcome`, диагностика вместо молчания); остались осознанные, задокументированные и отслеживаемые в BACKLOG остатки — отсутствие мультиреф-атомарности, невозврат repoint'а committed-ref и immediate-класс DDL-операций. (частично, P3) {#cas-005}

Находка датируется снапшотом 842f2b37b8f; после него именно эту область переработали TXN-ONE-PIPELINE (серия коммитов 2026-07-16: `5101a50dff5` спека, `39cf3279652` "moveDirectory tmp->final is a pure re-key; publish moves to commit", `d201e2e6586` "remove B151 early-publication machinery", `35a3335b19d` "commit-rollback spares pre-existing ref") и R3-фикс закрытия disk-транзакции в `renameParts` (`11077ee8ee0`, `a0a67d5fb50`). Разбор каждого утверждения по HEAD:

1. «Durable CAS mutations happen before commit()» — для write-path ЗАКРЫТО. `ContentAddressedTransaction.cpp:457` (HEAD): "[TXN-ONE-PIPELINE] This is the ONLY place a ref becomes durable — the tmp->final rename is a pure overlay re-key". Публикация всех частей идёт внутри `commit` (`ContentAddressedTransaction.cpp:493-496`), деструктор незакоммиченной транзакции лишь `abandon`'ит билды, никаких ранее опубликованных ref не существует (`ContentAddressedTransaction.cpp:119-121`: "No refs are published before `commit`"). ОСТАТОК (by-design): DDL/Remove-класс (`removeDirectory`/`moveDirectory` и verbatim-файлы) мутирует durable refs в момент вызова — это задокументированный контракт "everything-immediate" (`ContentAddressedTransaction.cpp:996-1002`), компенсируемый поверх committed-состояния, как upstream `rollbackPartsToTemporaryState`. Ровно этот остаток отслеживается в `docs/superpowers/cas/BACKLOG/formats-and-storage.md` (раздел `{#codecs-and-protocol}`, пункт "[TXN-ONE-PIPELINE follow-up] committed-ref DDL overlay", помечен HARD, отложен осознанно: "fixes no known motivating bug and has the highest regression risk"; interim-риск назван "narrow" и покрыт DDL-гейтами soak/stateless).

2. «Best-effort silent rollback / `dropRefIfMatches` swallows every error» — НЕ МОЛЧАЛИВЫЙ на HEAD. `Parts/PartFolderAccess.cpp:646-707`: при неудаче инкрементируется `ProfileEvents::CASRefRollbackBestEffortDropFailed` (`:698`), пишется лог "CA conditional rollback dropRefIfMatches failed ... the ref may remain live" (`:699-701`), а успешный откат аудируется событием `RefDrop` в `system.cas_log` (`:679-691`). Сам откат теперь ТОЧНЫЙ: ключуется на exact `CommitOutcome.manifest_ref`, а не на имя ref (`ContentAddressedTransaction.cpp:473-476, 508-510`), т.е. не может снести конкурентный repoint чужого writer'а — есть тесты `CASCommitRollback.RepointByOtherWriterSurvivesRollback` и `CASCommitRollback.AbsentBeforeDroppedPreExistingUntouched` (`src/Disks/tests/gtest_cas_parallel_commit.cpp:271,295`). Неудавшийся drop оставляет phantom-ref как unreferenced debris, который забирает GC — не потеря данных.

3. «Repointed committed ref is unrevertible» — ВЕРНО и на HEAD, но это осознанный fail-closed выбор, а не пропуск: `ContentAddressedTransaction.cpp:466-471` — "only refs that were ABSENT before we published them are rolled back. A ref that already existed is pre-existing data this commit must never destroy on its error path"; repoint (`created=false`) никогда не дропается (`:503-507`). Зафиксировано коммитом `35a3335b19d` как Audit-5 характеризация. Остаточный эффект: упавшая после repoint'а мультипартовая транзакция оставляет committed-ref в новом состоянии (пример из находки — второй `metadata_version.txt` при `REPLACE/ATTACH PARTITION`); операция при этом видимо падает и повторяется на уровне MergeTree — потери данных нет, откат repoint'а вслепую был бы опаснее (уничтожение чужого/предсуществующего состояния).

4. «Multi-part commits are not atomic (N independent publishes)» — ВЕРНО: публикация частей идёт последовательно, мультиреф-атомарного publish нет, что прямо задокументировано (`ContentAddressedTransaction.cpp:459-465`: "there is no multi-ref atomic publish ... would leave a PARTIAL commit") с компенсирующим best-effort откатом. Каждый publish индивидуально gate-checked и журналируется, остатки — GC-reclaimable debris; комментарий честно фиксирует, что это восстановление контракта wiring-слоя, не CAS-инвариант.

5. «Readers can observe aborted and intermediate states» — на HEAD НЕ ПОДТВЕРЖДАЕТСЯ на видимом читателю уровне. Порядок закреплён в `MergeTreeData::Transaction::renameParts` (`src/Storages/MergeTree/MergeTreeData.cpp:8995-9022`): диск-транзакция закрывается ДО Keeper-коммита ("every call site invokes renameParts BEFORE its external Keeper commit decision", `:9006-9009`, R3-фикс acked-then-lost), значит ref, опубликованный и затем откаченный, никогда не был зарегистрирован в Keeper/in-memory active set — запросы его не видят. Per-ref мутация ledger'а — один атомарный `appendRefOps` CAS (`PartFolderAccess.cpp:648-673`), «порванного» состояния одного ref читатель наблюдать не может. Остаточная наблюдаемость — phantom-ref после неудавшегося отката — видна только fsck/GC, логируется.

Итог: три из пяти столпов находки (ранняя durable-публикация write-path, «молчаливый» откат, откат-слепой-по-имени) исправлены после аудированного снапшота (TXN-ONE-PIPELINE 2026-07-16 + Task 3 точного отката + R3 `renameParts`); оставшиеся два (нет мультиреф-атомарности; repoint committed-ref не откатывается; immediate-класс DDL) — осознанные, задокументированные в коде решения с отслеживаемым follow-up в BACKLOG (`formats-and-storage.md`, `{#codecs-and-protocol}`, пункт "[TXN-ONE-PIPELINE follow-up] committed-ref DDL overlay"). Ничего требующего фикса до релиза сверх уже принятого в BACKLOG плана не осталось; приоритет P3 — остаток узкий, не data-loss, с работающей диагностикой и GC-восстановлением.

## CAS-002 — Отсутствие probe/condemn-check в `adoptEvidence` — сознательный дизайн §4 (manifest-trust, коммит `8fe6331a431`); заявленное окно data-loss на HEAD закрыто на каждом call-site (relink publish-then-confirm, `republishRef` commit-before-release, MergeTree-пиннинг источника при hardlink), остаток — принятый D4 trade-off с fsck-backstop. (by-design, P3) {#cas-002}

Фактические наблюдения finding'а верны, вывод (окно потери данных) — нет.

Что в finding'е ВЕРНО по коду на HEAD:
- `adoptEvidence` действительно не читает ни durable `Condemned`-маркер, ни сам объект: `Pool/CasPartWriteTxn.cpp:766-781` — "NO backend call (no HEAD, no GET, no PUT)", пишется tokenless dep с `adopted=true`.
- `promote` действительно принимает такой dep без probe: `Pool/CasPartWriteTxn.cpp:1123-1146` — "There is NO per-file probe on this path"; единственная проверка — `isTrustedAdopt` (`:1141`, реализация `:307-315`).
- Источника для re-upload у adopted-листа действительно нет (adopted-лист никогда не проходит `putBlob`/resurrect; resurrect-путь `:738-763` — только для токенованных загрузок из собственного источника писателя). Это верно и задокументировано ("A genuinely-absent adopted blob is an invariant violation caught by fsck, not here", `:1120-1122`, `CasPartWriteTxn.h:219-222`).

Почему это НЕ баг, а осознанный дизайн:
- Коммит `8fe6331a431` ("cas: opt §4 — manifest-trust relink adoption (no per-file probes, no tokens)", 2026-07-14) явно заменил прежний per-leaf HEAD+`.meta`-read (крупнейший потребитель чтений, ~68 HEAD + ~36 GET на парт) доверием к durable manifest edge, с явно принятым trade-off: "A genuinely-absent adopted blob moves from promote to fsck's reachable-but-absent scan (the accepted D4 trade-off)". Это соответствует зафиксированной позиции "relink trust model = обычный interserver-trust ReplicatedMergeTree" — не пересматриваю.
- "Bypasses EDGE-BEFORE-OBSERVE" — категориальная ошибка finding'а: EDGE-BEFORE-OBSERVE — дисциплина для путей, которые НАБЛЮДАЮТ backend (`observeAndAdmit`, `Pool/CasPartWriteTxn.cpp:327-329`). `adoptEvidence` не наблюдает ничего; его инвариант другой — EDGE-BEFORE-TRUST (`:1129-1140`): promote доверяет листу только после того, как precommit-edge этого же билда durable и re-доказан живым владельцем ("WPromote owner==bld", `:1105-1110`), после чего барьерно-активированный `+1` create-precommit'а держит in-degree >= 1 и GC (единственный удалитель) не может удалить blob.

Заявленное окно "adopt → precommit" (источник ретирован до того, как precommit-edge получателя стал durable) закрыто на КАЖДОМ call-site `adoptEvidence`:
1. Relink/fetch (`src/Storages/MergeTree/DataPartsExchange.cpp:1457-1533`): протокол publish-then-confirm построен ровно против этого окна — сначала `prepareAdoptFromManifest` (adopt + stage + precommitAdd, durable `+1`, `:1470-1473`), и только ПОТОМ read-only confirm к источнику; комментарий `:1464-1469` прямо формулирует: "This `+1` must be DURABLE before the source is asked anything". Промоут авторизует только ответ `PROVEN` (`:1494-1499`); ретирование источника до durable `+1` даёт `unproven` → fallback на byte fetch, ничего не публикуется.
2. `republishRef` (`Parts/PartFolderAccess.cpp:506-534`): `dropRef(src)` выполняется строго ПОСЛЕ `publishEntries(dst, ...)` (`:531-532`) — commit-before-release, источник остаётся committed всё окно.
3. Локальный hardlink/clone из committed-источника (`ContentAddressedTransaction.cpp:1186-1198`, диспетчер `adoptStagedBlob` `:223-245`): источник — живой парт локальной таблицы, удерживаемый на уровне MergeTree (мутация/клон держит `DataPartPtr`) на всё время транзакции, т.е. его committed ref не ретируется в окне → in-degree >= 1 → blob не condemnable. Плюс удаление у GC двухфазное (condemn → graduate в более позднем раунде): появившийся edge щадит blob ("the next fold sees the edge, d >= 1, spared", `:1113-1115`).
Триггер finding'а "last other owner is retired inside the adopt-to-precommit window" на hardlink-пути требует, чтобы таблица удалила исходный парт, из которого прямо сейчас хардлинкают — этого MergeTree-слой не допускает; для relink это ловит confirm.

Под-утверждение "downgrades an already-tokened dep to a tokenless trusted adopt": буквально верно, что `adoptEvidence` безусловно перезаписывает `deps[entry.ref]` (`Pool/CasPartWriteTxn.cpp:779`), но как downgrade это на HEAD недостижимо в реальных потоках: токенованные deps появляются только во время commit-time fan-out'а (`mergeBlobUploadResults`, `:264-305`), т.е. после всех staging-операций, а `prepareEntries` (`Parts/PartFolderAccess.cpp:471-494`) адоптит в СВЕЖИЙ build без prior deps. Даже при достижимости оба класса проходят promote под одним и тем же пином durable edge — последствий для safety нет (значение токена нигде не потребляется: `:742-746` "no consumer reads a dep token's VALUE"). Сам аудит (tla-fidelity-8) оценил это как Low.

BACKLOG/история:
- Прямого anchor'а по этой теме нет. Найден СМЕЖНЫЙ устаревший пункт `BACKLOG/performance.md:141` `[manifest-trust-promote-path]` — "not yet confirmed landed; verify against HEAD": на HEAD давно landed (`8fe6331a431`); пункт надо пометить DONE — это и есть P3-остаток этого вердикта (плюс, опционально, chassert против tokened→adopted перезаписи в `adoptEvidence`).
- История класса: copy-forward у promote-гейта существовал (`2b43cb913d1`, `3ab997c2bf9`) и был сознательно удалён в пользу manifest-trust (`8fe6331a431` удаляет `copyForwardFromCondemned`); EDGE-BEFORE-OBSERVE для токенованных листьев — `29145ba1c74`.
- Единственный признанный остаточный риск этого класса — не adopt-специфичный: неполный LIST у GC-fold'а ("What a `yes` does NOT prove", `DataPartsExchange.cpp:1383-1387`) — отдельно затрекан (LIST-trust verdict 2026-08-03, `BACKLOG/gc.md:17-33`), не предмет CAS-002.

## CAS-008 — Позиция подтверждена на HEAD: хеш выбираем (`blob_hash` = cityhash128|xxh3-128|sha256, дефолт cityhash128, фиксируется при создании пула), повторного хеширования на чтении нет — осознанное решение, не баг. (by-design, —) {#cas-008}

Факты на HEAD (ветка cas-gc-rebuild), код переехал из `CA/...` в `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`:

1. Настройка существует и хеш выбираем (Phase 1 + Phase 2 landed):
   - `ContentAddressedSettings.cpp:67`: `DECLARE(String, blob_hash, "cityhash128", "Pool blob content-hash function (cityhash128 | xxh3-128 | sha256); fixed at pool creation", 0)` — дефолт по-прежнему некриптографический `cityhash128`, но `sha256` доступен одной строкой конфига.
   - `Primitives/CasBlobDigest.cpp:33-45` (`parseBlobHashAlgo`): принимает `cityhash128|xxh3-128|sha256`, на неизвестное значение — исключение `BAD_ARGUMENTS`.
   - Алгоритм зафиксирован в метаданных пула: `Pool/CasPoolMeta.cpp:59` — `"CAS pool blob_hash mismatch: pool has {{{}}}; config requests {}"`; новый алгоритм в существующий пул допускается только явным opt-in `blob_hash_allow_new` (`ContentAddressedSettings.cpp:68`, `Pool/CasPool.h:63-69`). То есть тихо подменить алгоритм пула нельзя.
   - Реализация хеширования: `Primitives/CasBlobHashingWriteBuffer.h` — потоковые CityHash128 (chained, byte-identical со старым форматом), XXH3-128, OpenSSL SHA-256.

2. Повторной проверки хеша на чтении по-прежнему нет — и это осознанная позиция, не пропуск:
   - `ContentAddressedTransaction.h:291`: "the core never re-hashes payloads" (о read/copy-forward ядре).
   - `Pool/CasPartWriteTxn.cpp:78`: "The core otherwise never re-hashes payloads; any copy-forward ... with its own streaming digest" — повторное хеширование живёт только на путях ЗАПИСИ (дефис-модель "hash equality needs adversary model": re-hashing как identity-примитив при dedup-admit/copy-forward, см. `blobHashHexOneShot` в `Primitives/CasBlobHashingWriteBuffer.h:42-49` — "used by the re-hash and copy-forward path").
   - Зафиксированная позиция Филимонова (не пересматривается): re-verify на чтении несовместим с "CH does not slow down"; S3 сам хеширует объекты и даёт много девяток durability.
   - Целостность СЛУЖЕБНЫХ объектов при этом проверяется на чтении: `Formats/CasPartManifestFormat.cpp:293` (payload_digest манифеста пересчитывается и сверяется), `Formats/CasRecordStreamFormat.h:130` / `Gc/CasBlobInDegree.h:106` (`verifyAgainst`, `CORRUPTED_DATA`) — то есть "никакой верификации нигде" в формулировке находки неточно: не верифицируются именно тела блобов данных.

3. Что закрывает selectable hash: угроза chosen-collision относится только к некриптографическим 128-битным хешам; тот, кому важна модель злоумышленника с подобранными коллизиями, ставит `blob_hash = sha256` при создании пула — и вектор исчезает целиком (коллизий SHA-256 подбирать не умеют). Остаточная позиция "дефолт = cityhash128 + нет re-verify на чтении" — сознательный perf-выбор для доверенной среды записи.

4. BACKLOG: `docs/superpowers/cas/BACKLOG/performance.md:30`, раздел `{#read-write}` — пункт **[ch128ctx]** "slot-bound blob-hash middle tier": средний ярус `cityHash128 → ch128ctx → sha256`, привязывающий идентичность блоба к слоту записи, чтобы cross-slot chosen-collision dedup стал бесполезен при ~нулевой цене CPU. Это прямое продолжение той же темы, статус DESIRABLE. Отдельного пункта "re-verify on read" в BACKLOG нет — соответствует вето.

Итог: находка фактически точна (дефолт некриптографический, чтение не перепроверяет), но оба пункта — задокументированная осознанная позиция; выбираемый `sha256` уже закрывает коллизионную угрозу для тех, кому она релевантна. Дубликат ранее закрытых CAS-003/CAS-005 из предыдущего аудита.

## CAS-011 — Обход `CasRequestController` — факт и уже отслеживаемый residual, но заявление про обход fence ложно (fence-generation проверяется перед каждым durable PUT/DELETE и покрыто тестами); 100 попыток без сна — тормоз от live-lock, не политика повторов. (частично, P3) {#cas-011}

**(a) Что пишется этим путём.** `CasPlainObjects` — поверхность «плоских» (не content-addressed) объектов: (1) verbatim-файлы одной жизни namespace под `cas/ns/state/<life_id>/_files/` (`CasPlainObjects.cpp:91-128`, ключ — `Layout::namespaceFileKey`, `Formats/CasLayout.h:231-237`) — реально это `format_version.txt`, `uuid.txt`, табличные подкаталоги вида `deduplication_logs/...`, append-запись CSN мутации; (2) loose mountpoint-объекты, отзеркаленные по ClickHouse-пути под `roots/<server_root_id>/<path>` (`CasPlainObjects.cpp:130-152`, `CasLayout.h:292-300`) — startup write-probe, `clickhouse_access_check_*`. Единственные продакшн-вызовы — `ContentAddressedTransaction::writeFile` (ветка «не part-файл», `ContentAddressedTransaction.cpp:806-839`), rename/remove-ветки (`:1455-1480`, `:1636-1650`) и перенос файлов между жизнями (`:1273`). Ни данные парта, ни ref-журнал, ни манифесты, ни блобы этим путём не идут — README прямо описывает компонент как «the `roots/...` verbatim passthrough» (`ContentAddressed/README.md:102`).

**(b) «Обходит margin-checked fence» — ОПРОВЕРГНУТО в заявленном виде.** Обе durable-операции fence-gated: `const uint64_t admitted_generation = fence_generation_fn();` и `check_fence_or_throw_fn(admitted_generation)` внутри цикла непосредственно перед PUT (`CasPlainObjects.cpp:38,43`) и перед `deleteExact` (`:75,82`). `CasMountRuntime::checkFenceOrThrow` (`CasMountRuntime.cpp:97-113`) проверяет и `mayMutate()` (защёлка `lost` + дедлайн, `:80-84`), и совпадение генерации, и бросает типизированный transient. Проверка стоит на КАЖДОЙ итерации ретрая, а не только на первой, и это зафиксировано тестами: `src/Disks/tests/gtest_cas_fence_generation.cpp:220-236` (PUT не долетает), `:239-261` (DELETE не долетает), `:265-285` (`head_calls == 2` — перепроверка на второй итерации). Гейтинг добавлен коммитом `8b1df0cd914` («ca: fence-generation check on every durable-effect path»).
Верно лишь узкое: `checkFenceOrThrow`/`mayMutate` НЕ вычитают `attempt_timeout_ms + lease_safety_margin_ms`, как это делает ref-лейн через `refAppendFenceOk` (`CasMountRuntime.cpp:115-127`). То есть плоскую запись можно допустить, когда до истечения лизы осталось меньше одной попытки. Реальные последствия здесь ограничены условностью записи: `putIfAbsent`/`putOverwrite(…, head.token)` (`CasPlainObjects.cpp:46,51`) и `deleteExact(…, head.token)` (`:83`) — опоздавшая запись смещённой инкарнации получает precondition failure и НЕ затирает обновление преемника (lost update невозможен). Остаточный риск: (i) «воскрешение» — поздний `putIfAbsent` после того, как преемник удалил ключ; для мёртвой жизни namespace это только утечка хранилища (ключ включает life-id и структурно недостижим — `CasLayout.h:226-230`), внутри живой жизни путь rename (`ContentAddressedTransaction.cpp:1455-1457`) может оставить устаревшую копию по старому имени; (ii) поздняя посадка при неизменном токене — но это ровно та байтовая версия, которую писатель и намеревался записать.

**(c) «Обходит request controller» и «не разрешает неопределённые исходы» — ПОДТВЕРЖДЕНО, но это известный и отслеживаемый residual, а не новая находка.** `CasRequestController` подключён только к ref-лейну (`CasRefLedger.cpp:246,253,260,265` через `ref_request_controller` + `fence_ok_fn`); `CasPlainObjects` дергает `backend.head/putIfAbsent/putOverwrite/deleteExact` напрямую, без attempt-timeout, без операционного дедлайна и без exact-read разрешения `Unresolved` (семантика контроллера — `Backend/CasRequestControl.h:14-95`). Это дословно записано в бэклоге: `docs/superpowers/cas/BACKLOG/ref-protocol.md:20`, подпункт (c) — «bounded read/HEAD/LIST retries + startup validation for the non-ref plain-object paths (`casPutObject`/`casRemoveObject` still use the disk's default retry policy)». Уточнение к формулировке находки: «no attempt timeout» неверно — эти пути идут по ДЕФОЛТНОЙ политике S3-клиента (с её таймаутами и прозрачными ретраями); отсутствует именно single-attempt-дисциплина и разрешение исхода. Последствие: исключение уезжает наверх как «запись не удалась», хотя объект мог стать durable. Для last-writer-wins verbatim-файлов (`format_version.txt`, `uuid.txt`) это content-идемпотентно; единственный неидемпотентный случай — append с замороженным payload, и он уже отслежен как латентный (`BACKLOG/performance.md:31`, codex-26: единственный продакшн-аппендер — CSN мутации под per-table single-writer лизой, второго конкурентного аппендера нет).

**(d) «100 попыток с нулевым backoff» — факт верен, интерпретация нет.** `constexpr size_t MAX_CAS_ATTEMPTS = 100` (`CasPlainObjects.cpp:18`), цикл `:40-55` и `:77-87` без сна, при исчерпании — `ABORTED` («runaway live-lock brake», `:88`). Это тормоз от live-lock, а не политика повторов: каждая итерация делает сетевой HEAD (не спин на CPU), а новая итерация возможна только при реальном конкурирующем писателе на том же ключе, чего single-writer лиза + single-appender инвариант (`:27-32`) не допускают. Та же форма используется в ref-лейне (`CasRefCkpt.cpp:22` — «the same shape and for the same reason as `CasPlainObjects`'», но там с дедлайном); отсутствие дедлайна на плоском пути — часть того же residual (c).

**Итог.** Класс INTEGRITY завышен: подтверждённая часть (нет контроллера/дедлайна/exact-resolution, нет margin-вычитания) уже в бэклоге, а её худший исход ограничен утечкой ключа или устаревшей копией verbatim-файла, потому что все durable-операции условны по точному токену. Ключевое утверждение находки — «bypass the fence … never resolve» в смысле полного отсутствия fence-гейтинга — противоречит коду и тестам на HEAD. P3, не блокер релиза.

## CAS-009 — Механика описана верно (presence-only admit, ни один повторный аплоад/staged-body не пере-хэшируется), но digest-половина — by-design, а реальное окно порчи сводится к length-preserving расхождению локального scratch и уже отслеживаемой неатомарной записи emulated-бэкенда. (частично, P2) {#cas-009}

Общее замечание: номера строк в находке съехали (сейчас `observeAndAdmit` — `CasPartWriteTxn.cpp:327-442`, staging-хэш — `ContentAddressedTransaction.cpp:1862-1897`), но существо цитат подтверждается на HEAD. Явная проектная формулировка прямо в коде: «The core otherwise never re-hashes payloads; any copy-forward re-verification must use this convention» — `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:78-80`.

(a) dedup-admit существующего блоба — **presence-only, подтверждено**.
`CasPartWriteTxn.cpp:202-231`: при попадании в dedup-кэш (или при `deduplication_head_first_min_bytes`) делается один `head(key)`, и при `hr.exists` тело не передаётся вовсе — сразу `observeAndAdmit(..., hr)` и `return BlobUploadResult{..., DeduplicationCacheHit/HeadHit}`. То же на пост-412 ветке: `CasPartWriteTxn.cpp:683-686` — `if (!condemned) → observeAndAdmit(kind, ref, key, hr)`.
`observeAndAdmit` (`CasPartWriteTxn.cpp:350-441`) не читает тело: единственная проверка размера — защита от знакового переполнения `if (hr.size < header_len) → CORRUPTED_DATA` (`:358-361`), далее `logical_size = hr.size - header_len` (`:362`) и `return BlobDepRecord{kind, hr.token, logical_size, false}` (`:441`). Уточнение к находке: admit тут **даже не size-checked** — наблюдённый `logical_size` ни разу не сравнивается с `source.size`/`req.declared_size` вызывающего (`uploadBlobDetached` этого сравнения не делает, `CasPartWriteTxn.cpp:217-220`; единственная проверка `declared_size == source.size` — локальная, в фан-ауте, `ContentAddressedTransaction.cpp:1701-1705`). Т.е. усечённый объект по content-addressed ключу принимается как dedup-hit, и в манифест уедет ЕГО размер.
Вердикт по (a): digest-половина — **by-design** (позиция Филимонова из prev CAS-005: у S3 много «девяток» и он сам хэширует объекты; CAS не пере-хэширует прочитанное; ср. `feedback_hash_equality_adversary_model` — «в CA не строим skip-read сокращений» относится к обратному направлению: нельзя ЗАМЕНЯТЬ пере-хэширование чужим хэшем). Отсутствие size-guard — **открыто и уже трекается**: `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:31-33` («HARD: size guard at dedup-admit — `PartWriteTxn::observeAndAdmit` never compares the observed size against the caller's expected size … producing a durably unreadable part») плюс VERIFY-айтем `docs/superpowers/cas/BACKLOG.md:417` (`[dedup-presence-only-window-recheck]`). `git log -S"observeAndAdmit"` по каталогу CA: последний коммит, трогающий эту функцию, — `8adc1752a9c` / `bf4beddddf1`; фикса size-guard не приземлялось.

(b) staged/scratch body — **хэш считается на запись, при коммите байты НЕ пере-хэшируются; но усечение ловится**.
Хэш берётся потоково в `CaContentWriteBuffer`: `nextImpl` кормит `hashing` (`ContentAddressedTransaction.cpp:1862-1867`), `finalizeImpl` берёт `hash_hex = hashing->getHashHex()` и отдаёт его наружу через `on_finalized(hash_hex, size, temp_path)` (`:1869-1896`). `sink->sync()` на этом пути НЕ вызывается (`sync()` существует отдельно, `:1919-1924`, и на пути финализации staged-блоба не задействован) — т.е. локальный `*.tmp` не fsync-ается между хэшированием и коммитом.
При коммите байты берутся заново из того же файла: `ContentAddressedTransaction.cpp:292-296` — `source.open = [staging_key]{ return std::make_unique<ReadBufferFromFile>(staging_key); }`; далее `CasPartWriteTxn.cpp:611-620` стримит `[header][payload]` в `putIfAbsentStream` и проверяет ТОЛЬКО счётчик байт: `if (written != source.size) → LOGICAL_ERROR`. Пере-хэширования при commit нет ни в одной точке (единственные вызовы `blobHashHexOneShot` — `CasPartWriteTxn.cpp:88` `poolContentHash` для минта адреса и тесты).
Значит: усечение/укорочение scratch-файла (ENOSPC, kill) — **fail-closed** (счётчик байт расходится, ничего не публикуется); а **length-preserving** расхождение (bit rot, перезапись файла, порча в page cache) действительно опубликует байты под чужим адресом — это подтверждается.
S3-staging этого окна не имеет: хэш считается над теми же байтами, что уходят в staging-объект (`ContentAddressedTransaction.cpp:1817-1848`), а promote — write-once условный server-side copy (`CasPartWriteTxn.cpp:604-605` `promoteStaged`), локального read-back нет вообще (`ContentAddressedTransaction.cpp:274-289`).
Реально достижимое окно порчи на этом классе — не local scratch, а уже зафиксированное: неатомарная запись emulated/local бэкенда прямо в финальный content-addressed ключ + presence-only admit; трекается как **HARD `[disk-error-audit]`** в `docs/superpowers/cas/BACKLOG/formats-and-storage.md:36-46` («Presence-only admission + non-atomic local write is the ONLY corruption window the disk-error audit found»), с явной привязкой к size-guard как defense-in-depth. Смежный DESIRABLE — «fsck physical-size check for blob bodies» (`operability-and-introspection.md:34-36`).

(c) resurrect re-upload — **не пере-хэшируется, подтверждено; INV-1 при этом соблюдён**.
Локальный resurrect: `CasPartWriteTxn.cpp:751-759` — `auto payload = source.open(); … backend().resurrect(*payload, source.size, key, buildHeader())`; проверка — снова только длина, причём бэкенд считает байты в процессе и прерывается ДО публикации (комментарий `:755-758`). S3-resurrect: `CasPartWriteTxn.cpp:727-732` — `getStream(staging_key)`, `ignore(blob_header_len)`, `resurrect(*staged->stream, source.size, key, fresh_header)`; тело осуждённого объекта не читается никогда (`feedback_ca_resurrect_invariant` соблюдён), свежий `incarnation_tag` минтится в `buildHeader` (`:463-477`), что и защищает воскрешение от exact-token delete. Дайджест источника не перепроверяется — тот же класс, что (b), с той же зоной остаточного риска (length-preserving порча источника).

Итог: находка описывает механику корректно (никаких галлюцинаций по коду, кроме съехавших якорей и слишком мягкого «size-checked» в (a) — там нет и size-check), но «Impact: any length-preserving divergence» — это ровно тот остаток, который команда сознательно принимает на стороне объектного хранилища (by-design) и уже трекает на стороне локального/emulated пути (HARD × 2). Нового, нетрекуемого дефекта нет ⇒ P2, не pre-release-блокер.

## CAS-007 — Вложенный `server_root_id` (`a/b`) принимается валидацией, а владение в decommission определяется префиксом пути, поэтому `SYSTEM CAS DROP POOL MEMBER` на `a` удаляет пространства имён и объекты живого участника `a/b`. (подтверждено, P2) {#cas-007}

Проверка на HEAD `9b887ac8886757b99be8cd9f25cdc1115471cbd5` (ветка `cas-gc-rebuild`). Файлы переехали из снапшотных путей `CA/...` в `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/...`, символы найдены по grep.

(a) Валидация вложенного srid — многосегментный `server_root_id` разрешён СОЗНАТЕЛЬНО, никакого запрета на вложенность нет.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h:199-229` — `validateServerRootId` проверяет только: непустота, длина <= 255, отсутствие пустых сегментов (`//`, ведущий/замыкающий `/`), отсутствие `.`/`..`, отсутствие зарезервированных сегментов `_files`/`_manifests`. Слэши внутри разрешены явно: «It is a clean relative path» (`CasServerRoot.h:190`).
- Это подтверждено тестом: `src/Disks/tests/gtest_cas_mount.cpp:97` — `EXPECT_NO_THROW(validateServerRootId("shard-01/replica-a"))`, тогда как `:99-103` отвергают `/replica`, `replica/`, `a//b`, `a/../b`, `a/_files/b`. То есть вложенный srid — поддерживаемая форма, а не оплошность.
- Точка конфигурации: `ContentAddressedSettings.cpp:188-192` — отсутствие ключа даёт `NO_ELEMENTS_IN_CONFIG`, присутствующее значение уходит в `Cas::validateServerRootId`; никакой проверки «этот srid не является префиксом уже существующего участника пула» нет (grep по `ancestor`/`nested server_root`/`prefix of another` в поддереве CA и в `src/Disks/tests` — пусто).
- Документация просит уникальности, но не обеспечивает её: `docs/en/operations/storing-data.md:500-502` — «each one must own a distinct subtree». `a/b` внутри `a` формально не distinct, но это только текст.

(b) Как decommission выбирает жертву — по префиксу пути, и по каталогу, и по LIST'ам объектов.
- Выбор пространств имён: `Tools/CasDecommission.cpp:146` `const String victim_namespace_prefix = victim_srid + "/";` и `:150` `if (entry.ns.string() != victim_srid && !entry.ns.string().starts_with(victim_namespace_prefix)) continue;`. Пространства имён участника строятся как `<server_root_id>/<mirrored table dir>` (`ContentAddressedMetadataStorage.h:352-353`), поэтому пространства живого `a/b` (`a/b/db/t1`) начинаются с `a/` и ПОПАДАЮТ в выборку `owned_lives` при жертве `a`. Далее по ним вызывается терминальный `admin->dropNamespace(life)` (`CasDecommission.cpp:188`, `:194`) — необратимое удаление ссылок жертвы.
- Тот же префикс используется в проверке остаточного владения перед retirement: `CasDecommission.cpp:257-258`.
- Удаление объектов делается по LIST префикса, а префиксы вложены строго строково:
  - манифесты: `CasDecommission.cpp:220` `layout().casManifestsServerPrefix(victim_srid)` = `<prefix>/cas/manifests/a/` (`Formats/CasLayout.h:429-432`) — содержит `cas/manifests/a/b/...`;
  - staging: `CasDecommission.cpp:235-236` — `<pool_prefix>/staging/a/` содержит `staging/a/b/...`;
  - mountpoint-объекты: `CasDecommission.cpp:241-242` `layout().serverRootDataPrefix(victim_srid)` = `<prefix>/roots/a/` (`CasLayout.h:422-425`) — содержит `roots/a/b/...`, и удаляются они через `deleteListedPrefix` (`CasDecommission.cpp:47-83`), то есть точечными `deleteExact` по КАЖДОМУ перечисленному ключу, включая чужие.
- Слоты управления (`owner`/`epoch`/`mount`) удаляются по точным ключам (`CasDecommission.cpp:286-288`, `CasLayout.h:403-418`), так что слот `a/b` не сносится — но данные, манифесты и ссылки участника `a/b` уже уничтожены. То есть жертва «выживает как слот» и теряет содержимое: это худший вариант — участник остаётся живым и mounted, но его таблицы пусты/битые.
- Существующий тест закрывает только СОСЕДА, не потомка: `src/Disks/tests/gtest_cas_decommission.cpp:699` `TEST(CASDecommission, VictimNameMatchesOneCanonicalPathComponent)`, `:704-716` — проверяет, что жертва `victim` не выбирает `victim2`. Комментарий на `CasDecommission.cpp:143-145` формулирует ту же (частную) цель: «raw string prefixes such as `victim` must not select the distinct owner `victim2`; the slash makes `victim` one canonical path component». Именно из-за того, что слэш сделан канонической границей, вложенный `a/b` оказывается ВНУТРИ владения `a` — случай потомка ни тестом, ни кодом не рассматривается.
- Та же префиксная семантика владения — не локальная особенность decommission, а инвариант: `Pool/CasServerRoot.cpp:88-102` `serverRootSubtreeEmpty` считает `a` невладеющим только если нет записей `== a` и `starts_with("a/")` и пусты `cas/manifests/a/` и `roots/a/`. Так что модель «srid владеет всем поддеревом пути» согласована, и вложенный srid ей противоречит по построению.
- Асимметрия, подтверждающая, что это дефект, а не единая модель: маршрутизация relink-confirm использует СТРОГОЕ равенство srid — `ContentAddressedMetadataStorage.cpp:2021` `if (other_server_root_id.empty() || other_server_root_id != server_root_id) return false;` (комментарий `:2015-2017` прямо говорит, что `starts_with` намеренно НЕ используется). То есть в одном месте владение — равенство, в decommission — префикс.
- Частичное непреднамеренное смягчение: обратный порядок создания запрещён. Если `a/b` уже существует, монтирование `a` с отсутствующим owner-анкором падает: `CasServerRoot.cpp:145-149` (`serverRootSubtreeEmpty(a)` вернёт false из-за `roots/a/b/...` и записей каталога `a/b/...`) → `CORRUPTED_DATA` «has no owner anchor but its data subtree is non-empty». Опасен именно порядок «сначала `a`, потом `a/b`»: bootstrap `a/b` смотрит только на `roots/a/b/` и `cas/manifests/a/b/`, они пусты, и участник поднимается штатно. Никакого сканирования `gc/server-roots/` на предков/потомков при монтировании нет.
- Достижимость из SQL подтверждена: `src/Interpreters/InterpreterSystemQuery.cpp:1071-1075` — `SYSTEM CAS DROP POOL MEMBER` вызывает `Cas::decommissionPoolMember` с srid из запроса; предварительное `Pool::openForDecommission` (`Pool/CasPool.cpp:805`) требует лишь существования owner-анкора или mount-lease у `a`, то есть жертва `a` — вполне легальный живой/мёртвый участник.

(c) Работа «CAS pool-member decommission» — ЗАВЕРШЕНА и ответа не меняет.
- Код существует и живой: `Tools/CasDecommission.cpp`/`.h`, тесты `gtest_cas_decommission.cpp`, `gtest_cas_decommission_catalog_duties.cpp`.
- История: `git log -S "victim_namespace_prefix" -- src/` даёт `224aacd8eb9` «ca: close namespace removal and decommission duties» и `6a3dd6a9245` «ca: layout — opaque life ids split namespace streams from state»; последние правки файла — `70ca84c079c` «ca: decommission — catalog-exact duties; retirement fenced on owned entries» (2026-08-03). Именно эти коммиты и ввели/сохранили `starts_with(victim_srid + "/")`, то есть работа НЕ закрывает вложенность — она её и кодифицировала.
- Многосегментный srid отдельно поддерживался и починен: `b97847d32f9` «CAS: `listMounts` slices srid by `serverRootsPrefix` length — `rfind` truncated multi-segment srids» — ещё одно доказательство, что вложенные srid считаются валидной конфигурацией. Требуемая валидация введена в `1e157538806` «CA Phase0: required+validated server_root_id config + PoolConfig field» и запрета вложенности в неё не заложено.
- BACKLOG: grep по `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` даёт по decommission только другие пункты — `{#decommission-wrong-predicate}` (BACKLOG.md:308, расхождение предикатов «мёртв» между `cas_mounts` и `NoWait`), `[decommission-successor-mount-race]` (`BACKLOG/mounts-and-lifecycle.md:100`, гонка с преемником по mount/epoch), `{#life-epoch-monotone-per-server-root}` (там же:76), `{#loose-mountpoint-object-as-corrupt-namespace-file}` (operability-and-introspection.md:115), сценарная карта B200 (testing-and-ci.md:18). Ни один из них не про вложенные srid / префиксный выбор жертвы. Отдельного анкера под этот дефект НЕТ — при принятии находки его нужно создать.

Что реально осталось (ничем не закрыто): нет ни одного барьера между «вложенный srid валиден» и «владение = префикс пути». Достаточный минимальный фикс — fail-close в одном из двух мест: (1) при монтировании отвергать srid, у которого в `gc/server-roots/` есть предок или потомок с owner-анкором (один LIST по `serverRootsPrefix()` уже делается GC-гейтом, так что это дёшево), либо (2) в `decommissionPoolMember` перед любой деструктивной работой отказывать, если в `gc/server-roots/` найден слот-потомок `victim_srid + "/"`. Вариант (2) обязателен даже при наличии (1) — он защищает уже существующие некорректные конфигурации. Плюс тест-близнец к `VictimNameMatchesOneCanonicalPathComponent` на случай потомка (`victim` vs `victim/b`).

Оценка приоритета (свежая): P2. Для срабатывания нужна конфигурация, прямо противоречащая документированному требованию «distinct subtree», И ручная деструктивная команда оператора; один из двух порядков создания (`a/b` раньше `a`) уже отвергается fail-close в `claimOwnerOrThrow`. Но при срабатывании это молчаливая потеря данных живого участника без единого предупреждения, а фикс — один LIST и отказ, поэтому пункт должен быть заведён в BACKLOG и закрыт, а не забыт. PRE-RELEASE: нет (согласовано с P2).

## CAS-010 — Механизм реален — пустой `Token` в Native-режиме проходит все проверки и уходит на провод БЕЗ `If-Match`, но ни одного call site, где токен пуст по конструкции, на HEAD нет: это отсутствующий fail-closed guard, а не доказанный путь потери данных. (частично, P2) {#cas-010}

**(a) Код: пустой токен действительно даёт безусловную запись.**

`Backend/CasObjectStorageBackend.cpp:918-930` (`ObjectStorageBackend::putOverwrite`):
```
if (!mintingTypeMatches(expected.type))
    return {PutOutcome::PreconditionFailed, {}};
if (mode == Mode::Native)
{
    WriteSettings ws = conditionalWriteSettings();
    ws.object_storage_write_if_match = expected.value;
    return nativeConditionalPut(key, bytes, ws, meta);
}
```
Проверяется только `type`: `Backend/CasObjectStorageBackend.h:237` — `mintingTypeMatches(t) { return t == (mode == Mode::Native ? native_token_type : TokenType::Emulated); }`. А `Primitives/CasTypes.h:256-263` объявляет `TokenType type = TokenType::ETag;` по умолчанию, т.е. **default-constructed `Token{}` — это валидный по типу ETag-токен с пустым value**, и он проходит guard.

Дальше пустая строка теряется на проводе: `src/IO/WriteBufferFromS3.cpp:656-657` и `:746-747` — `if (!write_settings.object_storage_write_if_match.empty()) req.SetIfMatch(...)`. То же в Azure (`src/Disks/IO/WriteBufferFromAzureBlobStorage.cpp:195,258,338`). Итог: `putOverwrite(key, bytes, Token{})` → PUT вообще без precondition → безусловный clobber. Ровно так же `casPut` (`Backend/CasObjectStorageBackend.cpp:951-953`: `ws.object_storage_write_if_match = expected->value;`).

Это прямо противоречит контракту шва, записанному в `Backend/CasBackend.h:179-183` («backends that silently ignore the condition are …»), и асимметрично с verb'ом удаления: `deleteExact` пробрасывает токен в `S3ObjectStorage::removeObjectIfTokenMatches` (`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:495` — `request.SetIfMatch(etag)` **безусловно**), т.е. пустой токен на удалении заголовок всё-таки отправит, а на записи — молча выбросит. Прецедент того, что guard задуман, уже есть в самом файле: `Backend/CasObjectStorageBackend.cpp:1154-1157` (`resurrect`) — `if (token.empty()) throw ... FILE_DOESNT_EXIST ... "failing closed"`.

**(b) Call sites: реального пути с гарантированно пустым токеном не нашлось.**

Все источники токенов и их потребители:
- HEAD/GET: `nativeHead` (`:118-127`) → `hr.token = tokenForHead(metadata->etag)`, и `tokenForHead` (`Backend/CasObjectStorageBackend.h:146-150`) **не проверяет пустоту** (в отличие от `tokenForList`, `:154-159`, где `etag.empty() → nullopt`). Пустой токен здесь возможен только если хранилище вернуло объект без ETag: S3/RustFS/MinIO ETag на HEAD/GET возвращают всегда; `LocalObjectStorage` ставит `object_metadata.etag = <mtime ns>` (`ObjectStorages/Local/LocalObjectStorage.cpp:391,427`) — тоже непусто.
- `head()` для отсутствующего ключа возвращает `HeadResult{}` (`:696-702`), т.е. пустой токен. Но **все** call sites сначала проверяют `exists`: `Pool/CasPlainObjects.cpp:42-52` (`if (!head.exists) putIfAbsent; else putOverwrite(..., head.token)`), `Pool/CasPartWriteTxn.cpp:209-212`, `:661-673`, `Gc/CasGc.cpp:3567`, `Tools/CasDecommission.cpp:59`. Промахов не найдено.
- Результат записи: `tokenFromWriteResult` (`:859-881`) — в ETag-диалекте при отсутствующем/пустом etag ответа делает `auto hr = nativeHead(key); return hr ? hr->token : Token{};` — **это единственное место, которое штатно возвращает пустой токен как токен успешной (`Done`) записи**, без исключения. Такой токен уходит потребителям, и как минимум один из них хранит его для будущей условной записи: `Pool/CasServerRoot.cpp` — `SingleWriterSlot::recordWrite` (`:1291-1295`) кладёт `res.token` в `last_token`, а `:1186` и `:1337` делают `backend->putOverwrite(key, body, last_token)` (продление/retire mount-lease и watermark). Если бы `last_token` оказался пустым, продление lease стало бы безусловной перезаписью — т.е. пробой самой эксклюзивности single-writer'а. Но для этого нужны ДВЕ аномалии одновременно: ответ PUT без ETag (комментарий на `:843-858` фиксирует, что `WriteBufferFromS3` всегда присваивает `object_etag` на обоих success-путях) И 404 на немедленном follow-up HEAD только что записанного объекта. Ни одного call site, где токен пуст по конструкции, нет.
- Остальные условные записи берут `expected` из `get()`/`head()` или из `std::optional<Token>` (nullopt → put-if-absent): `Pool/CasBlobMeta.cpp:32-36` (`loadMeta(...).etag` = `got->token`), `Pool/CasRefCatalog.cpp:127,414`, `Pool/CasPoolMeta.cpp:92,155`, `Gc/CasGc.cpp:1076,4377,4504,4528-4603`, `Pool/CasServerRoot.cpp:272,398,430,663,996`.

Т.о. формулировка находки «the minted token is empty and the next putOverwrite sends no precondition» верна как механика, но её триггер («a write buffer that does not surface an ETag, or a 404 in the eventual-consistency window») — аномалия хранилища, а не штатный поток; демонстрируемого live-пути нет. Отсюда «частично»: это fail-open в самом чувствительном месте протокола (условная запись) и нарушение задокументированного контракта шва, лечится тремя строками (отклонять `expected.empty()` в `putOverwrite`/`casPut`-swap как `PreconditionFailed`/`Conflict`, и/или бросать в `tokenFromWriteResult`, как это уже делает `resurrect`), но не подтверждённая порча данных.

**(c) Emulated/InMemory backends отличаются — они fail-closed.**

`Backend/CasObjectStorageBackend.cpp:932-938`: emu-путь требует `emuExists(key)` и затем `tokenMatches(emuObserveToken(key), expected)` — сравнение точное (`Backend/CasObjectStorageBackend.h:163-166`, `observed == expected`), а `emuMintToken` (`:553-580`) никогда не возвращает пустое value (при пустом etag возвращает `std::to_string(++emu_seq)`). Значит пустой `expected` в Emulated всегда даёт `PreconditionFailed`. То же у `InMemoryBackend::putOverwrite` (`Backend/CasInMemoryBackend.cpp:170-185`: `it->second.token != expected → PreconditionFailed`). Следствие для тестов: **эту дыру нельзя воспроизвести на emu/in-memory backend'ах** — только Native-режим над S3-подобным хранилищем, и существующий gtest `CASObjectStorageBackend.NativeRejectsWrongDialectTokenBeforeTouchingTheWire` (`src/Disks/tests/gtest_cas_backend.cpp:974-998`) покрывает только неверный *тип*, но не пустое *значение*.

**Поиск по BACKLOG и истории:** в `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` анкера про пустой токен / безусловную условную запись нет (совпадения по «unconditional» — про resurrect `:422`, multipart `:445` и LIST-frontier `gc.md:22`, не про это). `git log -S "object_storage_write_if_match = expected.value" -- Backend/CasObjectStorageBackend.cpp` даёт только `592b9b83568` (git mv в слоёное дерево), `531adeebd6b` и его revert `ac7875d48ae` — то есть строка не менялась по смыслу и находка не исправлена.

## CAS-012 — Урегулированная позиция (by-design + docs) на HEAD держится, но док-половина закрыта только по versioning: lifecycle expiration, Object Lock/WORM и storage-class transitions в `docs/en/antalya/cas/` не описаны нигде, а Glacier-чтение падает сырым `S3Exception` без restore-and-retry и без классифицированной подсказки. (частично, P3) {#cas-012}

**Что урегулировано и подтверждается кодом (не пере-открываем).** Позиция Филимонова — «lifecycle expiration должен быть выключен так же, как versioning; plain bucket only; без admin-доступа это не детектируется» — на HEAD согласована с кодом: единственная проверка предусловий бакета — versioning, и только на GCS-диалекте. `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:53-56` («Only the Native, generation-dialect (GCS) combination has anything to check»), `:60-77` — inconclusive-ответ намеренно fail-open с `LOG_WARNING` («proceeding on the assumption that bucket versioning is OFF»), `:79-85` — подтверждённый `Enabled` бросает `NOT_IMPLEMENTED`. На AWS-диалекте versioning ловится не отдельным запросом, а по факту delete-marker: `CasProbe.cpp:217` (`if (d.created_delete_marker)`) в mount-time батарее и `Gc/CasGc.cpp:803` в раунде GC; поле определено в `Backend/CasBackend.h:102-110`. Никакого зонда/чтения bucket-конфигурации на lifecycle-правила, Object Lock, retention или storage-class в поддереве нет вообще — grep по `Lifecycle|ObjectLock|WORM|retention|StorageClass` в `ContentAddressed/` пуст. То есть «undetected and fail open» для этих трёх пунктов — факт, но by-design: детектор потребовал бы админских прав на бакет.

**Утверждение про «silently deletes live blobs» — уточнение.** Lifecycle-expiration действительно ничем не перехватывается заранее, но последствие не «тихое» в смысле неверных ответов: пропавший блоб на чтении даёт fail-closed путь — `ObjectStorageBackend::get` (`CasObjectStorageBackend.cpp:587-622`) конвертирует в `nullopt` только распознанный not-found (`isObjectNotFound`, `:323-343`: `NO_SUCH_KEY` / `"NoSuchKey"` / `FILE_DOESNT_EXIST`), «Any other error (network, auth, throttle, corruption) propagates unchanged — fail-closed». Потеря данных при lifecycle-правиле реальна, но это потеря самих байтов в бакете, а не деградация в тихий неверный результат.

**Док-половина закрыта частично.** `docs/en/antalya/cas/bucket-requirements.md` существует и подробен, но покрывает исключительно versioning: `bucket-requirements.md:26` (строка таблицы «No versioning / no delete markers») и `:29-31` («Bucket **versioning is not required** — in fact it must be **disabled** …»). Ни `lifecycle`, ни `Object Lock`, ни `WORM`/`retention`, ни `storage class`/`Glacier` в файле не встречаются (grep по всему `docs/en/antalya/cas/` даёт только versioning-упоминания: `architecture/backend.md:78`, `:88-90`, `roadmap.md:79`, `architecture/garbage-collection.md:116`). Так что заявленное в резолюции «add explicit user-facing bucket requirements» на HEAD **не выполнено** для трёх из четырёх пунктов. Это и есть actionable residual: добавить в `bucket-requirements.md` явный раздел «plain bucket only» — lifecycle-правил (expiration и transition) на префиксе пула быть не должно, Object Lock / retention / legal hold не поддерживаются, storage-class transitions запрещены; с объяснением почему (expiration удаляет живые блобы вне ведения GC; transition в Glacier делает блоб нечитаемым; Object Lock ломает exact-token delete и остановит reclaim; ни одно из этих условий не проверяется на mount).

**Glacier-половина (остаётся открытой).** Ни restore-and-retry, ни классификации `InvalidObjectState` в дереве нет: grep по `InvalidObjectState|Glacier|restore` в `ContentAddressed/` даёт только несвязанные тексты про «matching-sentinel restore» (`Pool/CasMountRuntime.cpp:368`, `Pool/CasPool.cpp:322`, `Gc/CasGc.cpp:212`), а в `src/IO/S3/` — только `storage_class_name` на записи (`copyS3File.cpp:155-157`, `:508-510`, `:725-727`). Поведение на транзитированном блобе: `readObjectRanged` бросает `S3Exception`, `isObjectNotFound` его не распознаёт, значит он пробрасывается наружу как есть — это корректный fail-closed (не «тихо отсутствует»), но пользователь получает сырую S3-ошибку без указания, что блоб в архивном классе и что делать. Это половина, которая остаётся: либо распознавать `InvalidObjectState` и бросать классифицированную ошибку CAS с явной подсказкой («блоб в архивном storage-класе; CAS не выполняет restore — верните объект в Standard»), либо ограничиться доком. Restore-and-retry внутри CAS я бы не делал (это молчаливый fallback с непредсказуемой задержкой в часы, против политики «avoid fallback paths»); минимум — классифицированное сообщение.

**Deny-DELETE.** Отдельной классификации `AccessDenied` на пути удаления нет: `ACCESS_DENIED` разбирается только в HEAD-зонде (`CasObjectStorageBackend.cpp:761-762` → `ProbeOutcome::AccessDenied`), `deleteExact` (`:984-1001`) маппит лишь precondition-failure в `TokenMismatch`, остальное пробрасывает. То есть Object-Lock/политика-deny на удалении = громкое исключение в раунде GC, а не тихая потеря — не data-loss, но диагностируемость слабая; логично закрыть тем же док-пунктом.

**BACKLOG.** Grep по `docs/superpowers/cas/BACKLOG.md` и `BACKLOG/*.md` на `lifecycle expir|object lock|WORM|glacier|storage class|InvalidObjectState|bucket requirement` — пусто. Тема нигде не отслеживается; нужен новый пункт (предлагаемый anchor выше) в `BACKLOG/docs-and-cleanup.md`, объединяющий: (1) док-раздел «plain bucket only» в `bucket-requirements.md`, (2) классифицированную ошибку на `InvalidObjectState`.

## CAS-013 — Механика подтверждена (допуск алгоритма CAS-поднимает пул-глобальный `min_reader_generation` до `G_BUILD` без записи блоба), но заявленного вреда сегодня нет: обратный порог формата уже равен `G_BUILD`, поэтому старые сборки и так не читают пул — дефект латентный, на будущее окно совместимости. (частично, P3) {#cas-013}

(a) Да, поднятие пул-глобальное и происходит в допуске алгоритма, а не при записи блоба.
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPoolMeta.cpp:87-92`:
```
        PoolMeta next = pm;
        next.algos_used.push_back(static_cast<uint8_t>(config_algo));
        std::sort(next.algos_used.begin(), next.algos_used.end());
        next.min_reader_generation = G_BUILD;
        const CasResult res = backend.casPut(key, encodePoolMeta(next), token);
```
`_pool_meta` — один пул-глобальный объект (`layout.poolMetaKey()`, `CasPoolMeta.cpp:120`), так что поле общее для всего пула. Путь вызова — `PoolMeta::createOrValidate` (`CasPoolMeta.cpp:108-128`), т.е. открытие/монтирование пула.

(b) Значение — именно `G_BUILD` текущей сборки (сегодня `9`, `Formats/CasFormat.h:73` → `constexpr uint32_t G_BUILD = 9;`), а НЕ генерация, которая ввела смешанные алгоритмы (генерация 2, см. комментарий `CasFormat.h:19-22`). В этой части модель права: это «свой номер сборки». Присваивание безусловное (не `max`), но опустить порог оно не может — `decodePoolMeta` фейлится закрыто на будущем значении (`Formats/CasPoolMetaFormat.cpp:174-177`), значит прочитанный `pm.min_reader_generation <= G_BUILD`.

(c) Нет, поднятие НЕ обусловлено фактической записью блоба новым алгоритмом: достаточно смонтировать диск с `blob_hash=<новый>` и `blob_hash_allow_new=1` — `admitOrValidate` пишет `_pool_meta` сразу при открытии пула. Без флага пул не трогается вовсе (`throwNotAdmitted`, `CasPoolMeta.cpp:56-62, 84-85`).

(d) Заявленный эффект «блокирует все старые сборки во всём пуле» сегодня НЕ достижим, и это главная поправка к находке. `decodePoolMeta` применяет жёсткий ОБРАТНЫЙ порог: `if (header.v < kCommittedRefFrontierGeneration) throw UNKNOWN_FORMAT_VERSION` (`CasPoolMetaFormat.cpp:110-117`), а `kCommittedRefFrontierGeneration = 9 == G_BUILD` (`CasFormat.h:91`). То есть любой читаемый этой сборкой пул написан ровно генерацией 9, и всякий свежесозданный пул уже штампуется `min_reader_generation = G_BUILD` при создании (`CasPoolMeta.cpp:152`). Для пула, созданного этой же сборкой, CAS-поднятие при допуске алгоритма — no-op. Старая сборка (`G_BUILD = 8`) отваливается раньше — на версии заголовка: `expectHeaderLine` → `checkCompatibility` → `UNKNOWN_FORMAT_VERSION` («… compatibility_version … at most …», `Formats/CasTextFormat.cpp:320-328`, `Formats/CasFormat.cpp:110-115`), так что до сообщения о `min_reader_generation` дело не доходит. Гейт по `mrg` даёт `UNKNOWN_FORMAT_VERSION` «pool requires reader generation N but this build supports at most M» (`CasPoolMetaFormat.cpp:174-177`) — он покрывается тестом `CASPluggableHash.ReaderGenerationIsRaisedToGBuild` (`src/Disks/tests/gtest_cas_pluggable_hash.cpp:689-720`) только через искусственный `G_BUILD + 1`.

Также неверна часть про «расхождение алгоритмов невидимо»: `algos_used` персистится в `_pool_meta` (`encodePoolMeta`, ключ `alg`, `CasPoolMetaFormat.cpp:84-95`) и печатается в отказе (`joinAlgoNames`).

Что остаётся (латентный дефект, не блокер): семантически порог должен подниматься до генерации, которая ввела несовместимость (для смешанных алгоритмов — 2, `CasFormat.h:19-22`), а не до `G_BUILD`. Как только появится реальное окно совместимости (обратный порог ниже `G_BUILD`, т.е. сборка `N` читает пулы генерации `N-1`), однократное монтирование с `blob_hash_allow_new=1` на одной ноде поднимет пул-глобальный порог до `N` и выкинет из ВСЕГО пула ноды генерации `N-1`, которым смешанный алгоритм ничем не мешал. Пока CAS pre-release и формат «только пересоздание» (нет миграции, нет персистентных данных) — вреда нет, поэтому P3 и PRE-RELEASE: нет.

Изменений по существу после аудита не было: последнее касание строки — `4ebbbe75d15` («feat(cas): PoolMeta.algos_used with flag-gated admission; pool-wide digest width deleted (Phase 3 T4)»), т.е. коммит, который эту механику и ввёл; HEAD `aa275736f82`. В `docs/superpowers/cas/BACKLOG.md` и `BACKLOG/*.md` записи про `min_reader_generation`/`blob_hash_allow_new` нет; ближайшая по теме — `{#cas-format-version-floor}` в `BACKLOG/formats-and-storage.md:74` (отсутствие нижнего порога по birth-generation в `checkCompatibility`), но это другой дефект. Рекомендуется добавить пункт в `BACKLOG/formats-and-storage.md` в раздел `{#codecs-and-protocol}`: «поднимать `min_reader_generation` до генерации причины несовместимости, а не до `G_BUILD`» — с отметкой, что задача становится актуальной ровно в момент первого понижения обратного порога ниже `G_BUILD`.

## CAS-014 — Классификатор действительно закрытый allowlist и не знает `primary.cidx`, `.mrk4`/`.cmrk4` и файлов вторичных индексов — но это не corruption: есть cap 1 MiB и спилл в blob, реальная цена — буферизация всего файла в памяти и двойная запись. (частично, P2) {#cas-014}

Что решает классификатор на HEAD (`a41d42ffe45`).
`Cas::partFileMustStayBlob` — `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:65-71`:
`if (file_name == "primary.idx") return true;` плюс суффиксы `{".bin", ".mrk", ".mrk2", ".mrk3", ".cmrk", ".cmrk2", ".cmrk3"}`. Никакого `else`-ветвления с логом/метрикой/отказом для незнакомого расширения нет — эта часть находки верна.

Решение — НЕ «inline vs blob» окончательно, а «стриминг vs буферизация»:
- `true` → `ContentAddressedTransaction.cpp:860` — потоковый `Cas::CaContentWriteBuffer` (локальный temp или S3-native staging), хеширование на потоке, память O(buf_size);
- `false` → `:923` `Cas::CaInlineWriteBuffer`, который копит ВЕСЬ файл в `std::string accumulated` (`:1946`, поле в `ContentAddressedTransaction.h:417`), а решение принимается только в `finalizeImpl`: `if (bytes.size() <= INLINE_CAP)` (`:932`) — запись Inline-строкой в манифест, иначе (`:952-980`) спилл в локальный temp-файл и обычный `stageBlobPartFile`.
Cap ЕСТЬ: `constexpr size_t INLINE_CAP = 1024 * 1024; /// 1 MiB` (`:98`), захардкожен (не настройка).

Промахи allowlist подтверждены на дефолтных настройках:
- `compress_primary_key` = `true` по умолчанию (`src/Storages/MergeTree/MergeTreeSettings.cpp:2121`), имя формируется как `"primary" + getIndexExtension(compress_primary_key)` → `.cidx` (`src/Storages/MergeTree/IMergeTreeDataPart.h:863`, `MergeTreeDataPartWriterOnDisk.cpp:115`). Значит ветка `file_name == "primary.idx"` при дефолтах МЁРТВАЯ, а `primary.cidx` идёт в память.
- `.mrk4`/`.cmrk4` реальны: `MarkType` (`src/Storages/MergeTree/MergeTreeIndexGranularityInfo.cpp:55-62`) даёт суффикс `4` для Compact с substream-марками, а `write_marks_for_substreams_in_compact_parts` = `true` по умолчанию (`MergeTreeSettings.cpp:405`). В allowlist их нет (в `src/Disks/tests/gtest_ca_transaction.cpp:283` `.cmrk4` уже используется как тестовый файл — и тест ничего про размещение не утверждает).
- файлы данных skip-индексов: `.idx` у обычных индексов (`MergeTreeIndices.cpp:71-72`), у text-индекса `.idx`, `.dct.idx`, `.pst.idx`, `.pos.idx` (`MergeTreeIndexText.cpp:1741-1747`), плюс `minmax_*.idx` — ни одного суффикса в allowlist.

Последствия (тут находка завышена, поэтому «частично»):
1. Corruption/непубликация НЕТ: спилл >1 MiB сохраняет инвариант «большое не лежит inline», объект/манифест корректны, ref-идентичность совпадает (`Cas::poolContentHash` — тот же mint, что у потокового пути, `:928`).
2. Реальная цена — пик памяти: весь файл держится в `std::string` до `finalize`. Для HNSW-графа `vector_similarity` или postings текстового индекса это сотни МБ–ГБ на файл (аллокации учитываются MemoryTracker, т.е. симптом — `MEMORY_LIMIT_EXCEEDED` на INSERT/merge, который на не-CA диске прошёл бы, а при неудачном лимите — реальный OOM), плюс двойная запись (память → temp-файл → upload) вместо стриминга. Для `primary.cidx` больших партов это десятки МБ на каждый билд парта.
3. `.mrk4`/`.cmrk4` — вред слабый: суффикс `4` существует только для Compact-партов, где марки — один файл на весь парт, поэтому его inline фактически совпадает с целью one-GET-open (документированный вред «полный fetch парта / потеря селективности» относится к per-column маркам Wide, а те (`.cmrk2`) в allowlist есть). Т.е. это не баг, а незадокументированное расширение inline-политики; при большом файле марок он всё равно сначала целиком буферизуется (пункт 2).
4. Документация предиката сама признаёт временность: `ContentAddressedTransaction.h:239-242` («`primary.idx`, which can be large (a size-threshold inlining of small primary.idx is a follow-up)») — но в списке остался НЕ тот вариант имени.

История: предикат добавлен в `c623713479f` («CA: add partFileMustStayBlob predicate (.bin/.mrk*/primary.idx)»), inline-путь — `27c5f790d19`, перенос в транзакцию — `41a70357f31`. Ревизия-снапшот `842f2b37b8f` в этом репозитории отсутствует (`fatal: Not a valid object name`), поэтому диффа «со снапшота» нет; по `git log -S partFileMustStayBlob` предикат с момента добавления не менялся.

Что остаётся: заменить закрытый allowlist на решение по размеру (ровно направление уже записанного пункта `[B121 / B202 / one-GET-open]`: «drop the file-type predicate, inline < ~512 KiB, `.bin` carve-out») либо, минимально, стримить всё, что не является inline-кандидатом по природе (никаких файлов с неизвестным расширением в память), и добавить наблюдаемость на «неизвестное имя файла парта». Отдельно стоит поправить мёртвую ветку `primary.idx` → `primary.cidx`/оба.

## CAS-015 — Ожидания на single-flight/лидере/восстановлении действительно без дедлайна и без отмены запроса, но каждое из них стоит за ограниченным по времени I/O (бюджет `CasRequestController` 90 с/16 попыток, бюджет восстановления 120 с, потеря mount-fence), поэтому «вечного» зависания нет — остаётся неотменяемость (`KILL QUERY`/`max_execution_time`) и суммирование ограниченных операций в минуты. (частично, P2) {#cas-015}

Нумерация строк в находке устарела (файлы сдвинулись); ниже — HEAD `9b887ac8886`. Все пути:
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.

1) Восстановление, ожидающий читатель — `Pool/CasRefLedger.cpp:1342-1348`:
`while (rt.recovery_in_progress) { ... rt.recovery_cv.wait(lock); ... }` — дедлайна нет, проверки отмены у ждущего нет.
ЧЕМ ОГРАНИЧЕНО ФАКТИЧЕСКИ: пробуждение гарантировано `SCOPE_EXIT` (`CasRefLedger.cpp:1355-1359`,
сбрасывает `recovery_in_progress` и делает `notify_all` на любом выходе, включая исключение), а сам
обход ограничен внешним retry-бюджетом `recovery_retry_budget_ms = 120000`
(`Backend/CasRequestControl.h:195`) с проверками `fence_ok_fn()`, `superseded_by_remount`,
`catalog_life_invalidated` и `fence_generation_fn() != admitted_generation` ПЕРЕД каждым сном
(`CasRefLedger.cpp:1487-1495`), плюс латч `cancelled`, который выставляет опрос внутри обхода.
Итог: «каждый читатель восстанавливающегося namespace висит вечно» — не подтверждается; висит на
время одного ограниченного восстановления.

2) Single-flight ref-очереди (конкурентные INSERT в одну таблицу) — `Pool/CasRefLedger.cpp:2044`
(`rt->cv.wait(lk);` внутри `while (!item->done)` в `appendRefOpsOnRuntime`): дедлайна нет, отмены нет.
ЧЕМ ОГРАНИЧЕНО: это не «follower навсегда» — как только `leader_active` освобождается, ждущий сам
становится лидером (`:1988-2010`); тенура лидера состоит из вызовов `CasRequestController`, каждый из
которых ограничен `operation_deadline_ms = 90000` / `max_attempts = 16` /
`attempt_timeout_ms = 5000` (`Backend/CasRequestControl.h:151-173`) и гейтится `fence_ok` — при полной
недоступности стора fence отваливается через ≈TTL и лидер падает fail-closed;
`completeOwnedItemsAndReleaseLeadership` гарантирует, что каждый принадлежащий лидеру item уходит
`done` (с ошибкой) на ЛЮБОМ выходе, т.е. ждущие не остаются осиротевшими.
ОСТАЁТСЯ: ожидание не отменяемо — во всём CA-дереве нет ни одной проверки `QueryStatus`/`isCancelled`
(grep по каталогу даёт только `shutdown_called` в `ContentAddressedMetadataStorage.cpp:396,626,892,1023`),
поэтому `KILL QUERY`/`max_execution_time` не прерывают ни этот `cv.wait`, ни `future.get()` ниже.

3) Shutdown-дренаж — ОПРОВЕРГАЕТ «нигде нет дедлайна»: `CasRefLedger.cpp:1877-1895` использует
`cv.wait_until(lk, deadline)` против ОДНОГО общего дедлайна, при таймауте ставит `timed_out` и
возвращает `false` (fail-closed: чистый farewell-маркер не пишется). Бюджет задаёт вызывающий:
`CasPool.cpp:880-881` = `attempt_timeout_ms + lease_safety_margin_ms` (≈7 с). Новые заявки при этом
отклоняются в том же критическом участке (`CasRefLedger.cpp:1972-1975`, `shutting_down`), т.е. DROP/
release не может «залипнуть» на дренаже.

4) Барьер отмены восстановлений — `CasRefLedger.cpp:1577-1600`: `recovery_cv.wait(slock, pred)` без
дедлайна, НО перед ожиданием всем таблицам выставляется `recovery_cancel_requested`
(`:1595-1596`), который обход опрашивает; это by-design join, ограниченный именно отменой.

5) `DROP TABLE`/`dropNamespace` — `CasRefLedger.cpp:4933-4939`: `rt->cv.wait(queue_lock, ...)` до
`!leader_active && pending.empty()`, без дедлайна/отмены; и `:5105-5110`
`publish_settle_cv.wait(...)` до нуля фоновых публикаторов. Оба ограничены чужим ограниченным I/O
(тенура лидера — см. п.2; публикатор видит потерянный fence и выходит без коммита). Тот же
`publish_settle_cv.wait` без дедлайна — в `enforceRefTableCacheBudget` (`:1710-1713`).

6) Single-flight part-folder — `Parts/PartFolderAccess.cpp:273-287`: `future.get()` без таймаута и без
отмены (только для `Freshness::CachedForLoad`); лидер делает `store->readManifestShared(...)`, и его
исключение прокидывается всем ждущим (`:298-303`), т.е. висят они ровно на одном GET манифеста.
ЧЕМ ОГРАНИЧЕНО: обычным ретрай-политиком S3-диска, а НЕ `CasRequestController` — это ровно известный
остаток `[timeout-retry RFC residuals]` пункт (c) в `docs/superpowers/cas/BACKLOG/ref-protocol.md:20`
(«bounded read/HEAD/LIST retries … non-ref plain-object paths still use the disk's default retry
policy»). Именно здесь заявленная «неограниченность» ближе всего к правде.

7) `CasPool.cpp`: `writer_cleanup_cv.wait(lock, ...)` (`:1288-1293`) ждёт снятия флага `draining`
другого дренажа — без дедлайна, ограничено его I/O. Ожидание истечения аренды при монтировании
(`claimMountAwaitingExpiry`, `CasPool.cpp:658-661`, `:1105`) — ограниченный опрос по `ttl_ms` /
`poll_interval_ms` через `mount_runtime.waitSleep`, с явным fail-closed на `LiveDoubleStart`/
`ForeignOwner` (`:687-695`). Утверждение находки про «`remount_mutex` удерживается через опрос
истечения и два ожидания quiescence» верно по факту удержания (`CasPool.cpp:998`
`std::lock_guard serialize(remount_mutex)`), но эти ожидания ограничены (TTL-опрос + отменяемый
join `cancelRecoveriesAndAwaitQuiescence`, см. п.4; комментарий `:1154` это и фиксирует).

ЧТО ОСТАЁТСЯ (собственно дефект, P2):
(a) ни одно из ожиданий не отменяемо запросом: нет проверки `QueryStatus::isCancelled`/
    `checkTimeLimit`, так что `KILL QUERY` и `max_execution_time` не действуют на INSERT/SELECT,
    припаркованный на CA-барьере;
(b) плановые read/HEAD/LIST-пути (в т.ч. `PartFolderAccess` single-flight) не заведены под
    `CasRequestController` — BACKLOG `[timeout-retry RFC residuals]` (c), без anchor,
    `docs/superpowers/cas/BACKLOG/ref-protocol.md:20`;
(c) латентность складывается: одна тенура лидера/один обход восстановления может состоять из многих
    операций по 90 с (комментарий `Backend/CasRequestControl.h:189-193` прямо это признаёт: «one
    recovery attempt may itself burn ~90s inside a single seal PUT»), т.е. наблюдаемая задержка
    измеряется минутами, а не бесконечностью. Смежный уже зафиксированный дизайн-вопрос —
    `[fence-window blast radius]` в `docs/superpowers/cas/BACKLOG/mounts-and-lifecycle.md:21`
    (ограниченное ожидание remount на пути записи).
Проверенный anchor `{#gc-budgets-need-a-deadline}` (`docs/superpowers/cas/BACKLOG.md:391`) к этим
местам НЕ относится — он про отсутствие wall-clock дедлайна у раунда GC, другие сайты.
Фиксов, закрывающих именно эти ожидания, в истории нет; релевантные коммиты, ограничившие соседние
пути: `fb0963cb408`/`ad6c0cac3b7`/`3ee4e296da9` (retry-бюджет восстановления) и `2332baf8250`
(дренаж ref-lanes с дедлайном при чистом release).
Не P1/не pre-release: нет потери данных и нет вечного зависания; это качество отмены и латентность
под деградированным стором.

## CAS-018 — Головной механизм (утечка лидерства в ref-очереди) уже закрыт единой точкой выхода и тестами; из шести якорей подтверждаются только теоретические аллокации в `noexcept`/деструкторах под лимитом памяти, а «renewal фенсит маунт» — прямо неверно. (частично, P3) {#cas-018}

**(a) Где ставится и снимается лидерство ref-ленты — RAII или руками (главное утверждение).**

На HEAD лидерство ставится вручную, но окно броска закрыто *конструктивно*, а не RAII-объектом:

- `Pool/CasRefLedger.cpp:2019` — `rt->leader_active = true;` выполняется ПОСЛЕ всех бросающих операций: единственная аллокация постановки (`owned_items.push_back(item)`) сделана раньше, под `lk`, в собственном `try/catch`, который при броске снимает item из `pending` и пробрасывает (`CasRefLedger.cpp:2006-2017`). Комментарий там же (`:1998-2005`) фиксирует это как намеренное свойство: «becoming leader contains NO throwing operation once `leader_active` is set».
- `CasRefLedger.cpp:2022-2029` — вызов `runRefQueueLeader` обёрнут в `catch (...)`, который НЕ пробрасывает, а сохраняет исключение в `flush_exception`.
- `CasRefLedger.cpp:2039` — `completeOwnedItemsAndReleaseLeadership(...)` вызывается безусловно, вне `try`, на нормальном и на исключительном пути; она и только она сбрасывает `leader_active` (`:2099`) и делает `cv.notify_all()` (`:2100`). Это же зафиксировано в контракте объявления: `Pool/CasRefLedger.h:1160-1169` («This is the single authority that resets `leader_active` on any exit from the leader loop»).

То есть «released outside RAII → throw между set и release» на HEAD **невозможно**: между `:2019` и `:2039` нет ни одного пути, который бы пробросил исключение мимо релиза.

Исторически дефект был реален и уже исправлен: коммит `79c07d6cc3d` «cas: make the ref-lane exception-safe (no stranded pending item on leader throw)» (позже уточнён `cc5387ea0fd`, `028c3c865e7`). Регрессионное покрытие есть и оно именно про этот класс: `src/Disks/tests/gtest_cas_ref_lane_exception_safety.cpp:45` (`SoloLeaderThrowBeforeCarveDrainsOwnItem`) и `:79` (`FollowerNeverRunsStrandedLeaderClosure`) — вплоть до детерминированной парковки лидера в pre-carve хуке. Так что подпункт «deadlocks the namespace forever» как заявленный механизм — **исправлено**.

**(b) Что осталось реально: сама функция релиза может бросить.**

`completeOwnedItemsAndReleaseLeadership` НЕ помечена `noexcept` (`CasRefLedger.h:1166`) и на fail-closed ветке аллоцирует: `std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR, "...{}...", ns.string()))` c `fmt::format`-подстановкой и захватом стектрейса — `CasRefLedger.cpp:2085-2090`. Если эта аллокация упадёт (`MEMORY_LIMIT_EXCEEDED` от трекера или `bad_alloc`), исключение уйдёт наружу до `rt->leader_active = false` (`:2099`), и тогда:
- все последующие/уже ждущие вызовы висят навсегда — `rt->cv.wait(lk)` в `CasRefLedger.cpp:2044` без таймаута, а нового лидера взять нельзя (`if (!rt->leader_active)` на `:1985`);
- шатдаун-дренаж это заметит, но лечить не сможет: `drainRefLanesForShutdown` ждёт `rt->pending.empty() && !rt->leader_active` по общему дедлайну (`CasRefLedger.cpp:1883-1889`), вернёт `timed_out` → `~Pool` уйдёт в fail-closed «без терминальной операции» (`Pool/CasPool.cpp:880-888`, `Pool/CasMountRuntime.cpp:524-533`) — следующий маунт будет реклеймить медленным observation-путём.

То есть заявленное последствие («namespace deadlocked forever») достижимо, но требует ОДНОВРЕМЕННО: (i) выхода лидера без исключения при незакрытом owned-item — состояние, которое сама эта ветка описывает как «не должно случаться» и существует только для fail-close, и (ii) отказа аллокации ровно в этот момент. Нормальный путь релиза аллокаций не содержит (`owned->error = flush_exception` — копия `exception_ptr`, `std::erase(rt->pending, owned)` — без аллокаций). Дешёвое ужесточение, если брать: сначала `leader_active = false` + `notify_all`, потом достройка ошибок; либо `noexcept` на функции (тогда отказ станет fail-fast вместо вечного дедлока), либо заранее собранный статический fallback-`exception_ptr`.

**(c) Деструкторы и `noexcept`-пути с аллокацией — что подтверждается.**

- `Gc/CasGcPhaseTimer.h:52-75`: деструктор (неявно `noexcept`) строит `GcPhaseRecord`, а затем в цикле по всем счётчикам делает `rec.profile_events.emplace(String(ProfileEvents::getName(e)), ...)` (`:65-70`) — это аллокации ВНЕ единственного `try`, который прикрывает только `sink(rec)` (`:74`). Комментарий на `:29-33` прямо говорит, что таймер по замыслу срабатывает и во время разматывания упавшего раунда. Итог: под глобальным лимитом памяти теоретически `std::terminate`. Подтверждается как hardening-нит; исправление — обернуть всё тело деструктора.
- `Backend/CasProbe.cpp`: лямбда `cleanup` объявлена `noexcept`, но `for (const auto & k : {key, cas_key})` формирует `initializer_list<String>` (две копии строк — аллокации) ВНЕ внутреннего `try`; сам HEAD/`deleteExact` внутри уже обёрнуты. То же в `probeConditionalCopy`'s `cleanup`. Реально достижимо только под лимитом памяти на маунте; практически ничтожно.
- `~Pool` (`Pool/CasPool.cpp:861-889`) на HEAD **уже не такой**, как в якоре (`CasPool.cpp:562-571` из снапшота): там больше нет ни логирования, ни голого join — только `mount_runtime.stopRemountThread()`, `ref_ledger.drainRefLanesForShutdown(...)` и `mount_runtime.finishTeardown(drained)`. Ни один из вызовов не `noexcept`, и внутри `finishTeardown` есть аллоцирующие `LOG_WARNING` (`Pool/CasMountRuntime.cpp:529-531`) и незавёрнутый `mount_keeper->stopBackground()` (`:532`); отказ маунт-лизы уже корректно подавлен через `tryLogCurrentException` (`:515-522`). Так что «logs and joins with no handler» — устарело; остаётся тот же теоретический класс, что и выше.
- `Pool/CasServerRoot.cpp` — утверждение «успешное продление лизы может зафенсить маунт, потому что post-commit callback бросает» **неверно**: `onRenewSucceeded()` вызывается из цикла продления внутри `try { ... } catch (...)` именно с этой мотивацией — «A notification hook cannot be allowed to stop the already-renewed lease loop» (`Pool/CasServerRoot.cpp:1474-1483`); симметрично обёрнут и `onRenewFailed()` (`:1446-1454`). Сам `onRenewSucceeded` (`:1038-1046`) только читает `on_renew_ok` и вызывает его; дедлайн уже обновлён в `onRenewCommitted` (`:1029-1036`). Not-a-bug.
- `Pool/CasPartWriteTxn.cpp` (якорь `:551-572` → на HEAD `stageManifest`, `:880-908`): post-durable окно действительно есть — после подтверждённого PUT тела манифеста (`:880`) идут бросающий `EventEmitter{...}.emit` (`:895-904`; `Primitives/CasEvent.h:99-106` — «any exception from the builder or the store is propagated») и аллоцирующие `staged_manifests.push_back(id)` / `staged_manifest_ids.insert(id)` (`:906-907`). Но последствие — не потеря данных, а неучтённое тело манифеста, и оно уже покрыто по замыслу: та же функция на `:887-893` фиксирует, что незанайменное тело — «inert unreferenced debris for the orphan-manifest sweep» (`Gc/CasOrphanManifestSweep.*`), а ordinal уже сдвинут, так что повторный stage ключ не переиспользует. Т.е. «durable manifest body can end up with no in-memory record» — верно буквально, но это мусор, который сметает GC, а не корректностный дефект.

**(d) Реальное vs теоретическое, и что смотрел ещё.**

RAII в поддереве используется широко (`SCOPE_EXIT` встречается в 10 файлах CA, в `CasRefLedger.cpp` — 6 раз), а второй «латч лидерства» — GC — тоже закрыт: `i_am_leader` — это подсказка над авторитетной durable-лизой `gc/state` (`Gc/CasGcScheduler.cpp:86-92`), и он снимается и в `catch (...)` цикла (`:344-347`), и на terminal-выходе (`:284`).

В `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` этого класса (noexcept/деструкторы/аллокация под лимитом памяти) нет; ближайшее по духу — `BACKLOG/operability-and-introspection.md:53` («MINOR: destructor-`abandon` live-epoch precommit debris»), это про другое.

Итого: заявленный головной механизм исправлен и покрыт тестами; «renewal фенсит маунт» — неверно; «манифест без записи» — верно, но by-design и подметается GC; остаётся один узкий класс hardening-нитов (аллокация в релизе лидерства, в деструкторе `GcPhaseTimer`, в `noexcept`-лямбдах `CasProbe`, в `~Pool`) — все достижимы только при отказе аллокации в конкретном микроокне, ни одного наблюдённого случая. Это P3: аккуратно закрыть одним проходом (обернуть тела, `noexcept` там, где выход всё равно недопустим, сбрасывать латч ДО достройки диагностики), не блокируя релиз.

## CAS-019 — Ключ single-flight действительно только `ns+ref` и post-wait проверки manifest id нет, но каждый выданный view внутренне консистентен (один манифест), сингл-флайт работает только на stale-терпимом `CachedForLoad`, так что последствие — сдвиг на один репойнт, а не смешение двух манифестов. (частично, P2) {#cas-019}

**(a) Каков ключ single-flight на HEAD.**

Только пространство имён + имя рефа, без manifest id. `Parts/PartFolderAccess.h:34`: `String cacheKey() const { return ns.string() + '\0' + ref; }`; карта — `Parts/PartFolderAccess.h:383-384` (`std::unordered_map<String, std::shared_future<...>> inflight`); поиск/вставка/удаление — `Parts/PartFolderAccess.cpp:277`, `:283`, `:291`. Тот же `cacheKey` используется и для retained-кэша (`:171`, `:229`). Так что подпункт (a) находки — **подтверждается**.

**(b) Валидирует ли ожидающий полученный view против своего manifest id.**

Нет. Ведомый выходит немедленно: `Parts/PartFolderAccess.cpp:286-287` — `if (!leader) return future.get();` — и результат уходит из `buildView` в `getView` (`:215`) без сравнения с `resolved->manifest_id`. Post-wait проверки, которая сделала бы находку неактуальной, на HEAD нет. Это тем заметнее, что тот же метод сам себе объявляет инвариант «view соответствует свежему резолву» и на кэш-хитах его ЭНФОРСИТ: `:176-191` (`cached->manifestId() == resolved->manifest_id`, иначе `CASPartFolderViewValidationMismatches` и перестройка) и `:197-213` для `ForceFresh` с политикой валидации. Сингл-флайт — единственная ветка `CachedForLoad`, которая этот инвариант обходит.

Насколько окно реально: `resolveRef` на HEAD **не** отдаёт устаревшее состояние — параметр `allow_stale` больше ничего не выбирает, читается авторитетная in-memory ref-таблица единственного пишущего маунта (`Pool/CasRefLedger.cpp:275-282`, `:313-320`). Значит расхождение manifest id между лидером и ведомым — это настоящий репойнт, попавший в окно (порядок допускает и «ведомый резолвил новее, получил старый view», и обратный случай: резолв лидера строго раньше его вставки в карту, но относительно резолва ведомого не упорядочен).

**(c) Что из «CAS part-folder cache spec» приземлилось и меняет ли это ответ.**

Приземлилось: коммит `37135e0075c` «CAS wiring: retained part-folder views with validate-on-hit, single-flight, write-through erases» — он же ввёл и `inflight` (`git log -S"inflight.emplace"`). Из его же сообщения: «correctness rests on validate-on-hit, not on catching every mutation site» — то есть протокол валидации задумывался на уровне кэш-хита, а координация одновременной холодной постройки ключевалась по `cacheKey` без manifest id и в спеке отдельно не разбиралась. `ForceFresh must HEAD the body` тоже приземлилось и здесь работает в нашу пользу: сингл-флайт применяется ТОЛЬКО к `CachedForLoad`, а `ForceFresh`/`StrictValidate` всегда строят собственный view со своим обязательным HEAD (`Parts/PartFolderAccess.cpp:267-270`). Существующий тест `CASPartFolderAccess.SingleFlightColdBuild` (`src/Disks/tests/gtest_cas_part_folder_access.cpp:1071-1098`) проверяет только «один GET на всплеск», идентичность манифеста у ведомых он не утверждает. В `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` про keying сингл-флайта нет ничего; смежное — `BACKLOG.md:415` `{#part-folder-validate-never-gating}` (про значение `part_folder_validate=never`) и `BACKLOG/gc.md:55` (audit-gap в `repointRef`).

**(d) К чему приводит «чужой» view — неверные результаты или ретрай.**

Заявленное в находке последствие («sizes and blob references from one manifest, presence decisions from another») — **неверно**: `PartFolderView` неизменяем и целиком построен из ОДНОГО разделяемого декода манифеста (`Parts/PartFolderAccess.h:71-100`, поля `manifest_id`/`manifest_body`), все ответы — чистые функции от него (`findFile`/`fileSize`/`listChildren`, `.cpp:124-134`), а `getView` не смешивает `resolved` с содержимым view (из `resolved` дальше используется только `emitResolveEvent`, `.cpp:244`, `:248-262`). Смешения двух манифестов внутри одного ответа не бывает.

Реальное последствие — точечный сдвиг во времени: read-путь загрузочного окна может получить view на один репойнт старше/новее того, что он сам зарезолвил. Практический худший случай — пропавший или ещё не появившийся per-part файл в момент конкурентного репойнта: `existsFile` вернёт false (`ContentAddressedMetadataStorage.cpp:1391-1392`), а `getStorageObjects` бросит `FILE_DOESNT_EXIST` («file {} not in manifest of {}», `ContentAddressedMetadataStorage.cpp:1867`), то есть ошибка запроса/ретрай, а не тихо неверные байты. Экспозиция ограничена: все чувствительные к read-after-write пути идут через `ForceFresh`, который не коалесцируется — `ContentAddressedTransaction.cpp:339`, `:1190`, `:1600-1601`, `ContentAddressedMetadataStorage.cpp:2094` (там же комментарий `:2098`: «The token names the manifest THIS view resolved» — и это именно `ForceFresh`). Плюс каждый коммит-примитив делает write-through `eraseView` (`Parts/PartFolderAccess.cpp:358`), на что явно опирается комментарий `ContentAddressedMetadataStorage.cpp:1387-1390`, — сингл-флайт как раз тот случай, где эта гигиена не помогает, потому что постройка уже стартовала до erase.

**Что реально осталось.** Один узкий дефект координации, чинится тривиально и без изменения протокола: либо добавить `resolved.manifest_id.ref` в ключ `inflight` (тогда постройки под разные манифесты не сливаются, а всплеск одинаковых по-прежнему коалесцируется), либо оставить ключ и после `future.get()` сверить `view->manifestId() == resolved->manifest_id`, перестроив view при несовпадении (та же логика, что уже есть на кэш-хите, плюс инкремент `CASPartFolderViewValidationMismatches`). Тест — детерминированный: припарковать лидера в постройке, сделать репойнт, отпустить ведомого и потребовать совпадения `manifestId` с его резолвом. P2: не блокирует релиз (лечится ретраем, wrong-bytes не даёт), но это нарушение собственного инварианта класса и его стоит закрыть.

## CAS-024 — Конфигурация «два CAS-диска на одном пуле с одинаковым `server_root_id`» не доживает до записи: второй диск падает на mount-протоколе с `ABORTED` (live double-start), поэтому пути потери данных при `MOVE PARTITION TO DISK` нет. (not-a-bug, P3) {#cas-024}

Замечание про якоря: анкоры находки (`CA/ContentAddressedMetadataStorage.cpp:886-889`, `:903-934`, `CA/ContentAddressedSettings.cpp:119-137`) устарели и по путям, и по номерам строк. На HEAD это `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:1267-1273` (`liveNamespace`), `:1291+` (`route`), `ContentAddressedSettings.cpp:172-196` (`validate`).

1) Механическая часть находки верна. `liveNamespace` действительно не содержит никакой идентичности диска:
`ContentAddressedMetadataStorage.cpp:1272` — `return Cas::RootNamespace{serverPrefix() + "/" + Cas::mirroredArchiveNamespace(table_uuid)};`, где `serverPrefix()` — это только `server_root_id`. `route` (`ContentAddressedMetadataStorage.cpp:1291-1335`) выводит `ns` из `liveNamespace(p.table_uuid)` и `ref` из имени каталога куска. То есть два диска с одинаковыми `server_root_id` + одинаковым пулом при одинаковом UUID таблицы дали бы один и тот же `(namespace, ref)`.

2) Валидация конфигурации коллизию действительно НЕ ловит — здесь находка тоже права. `ContentAddressedSettings::validate` (`ContentAddressedSettings.cpp:172-196`) проверяет только `gc_interval_sec`/`gc_shards`, обязательность `server_root_id` и его форму (`Cas::validateServerRootId`, `Pool/CasServerRoot.h:189-230`). Это per-disk проверка, никакого реестра уже смонтированных `(pool_prefix, srid)` в дереве нет (грепы по `registry`/`getOrCreate`/`already mounted` в `ContentAddressedMetadataStorage.cpp` и `Pool/CasPool.cpp` пусты; каждый диск открывает СВОЙ `Pool` — `ContentAddressedMetadataStorage.cpp:780` `view.pool = Cas::Pool::open(...)`, единственная точка вызова `Pool::open` вне decommission/тестов).

3) Но конфигурация отбивается на mount-протоколе, а не на конфиге, и именно поэтому вся цепочка потери данных недостижима.
- `ContentAddressedMetadataStorage::startup` (`:784-798`) вызывает `openPoolView()` → `Pool::open`, а тот на writable-открытии обязательно идёт в `mountWritable` (`Pool/CasPool.cpp:519-520`, вызов на `:518`).
- Owner-claim коллизию НЕ отбивает: у обоих дисков один и тот же `ServerUUID`, поэтому `claimOwnerOrThrow` уходит в fast-path равного UUID — `Pool/CasServerRoot.cpp:125-129` (`if (owner->server_uuid == our_uuid) { throwIfOwnerRetired(...); return; }`). То есть пункт (a) находки про идентичность — не тот барьер.
- Отбивает mount-lease (liveness). Второй диск получает СВЕЖИЙ `writer_epoch` (`Pool/CasPool.cpp:589-591`, `allocateWriterEpoch`), поэтому в `claimMount` попадает в ветку «same uuid, DIFFERENT epoch»: `Pool/CasServerRoot.cpp:408-453`. Аренда первого диска не `gc_fenced`, не несёт clean-marker (`min_active == UINT64_MAX`) и не является `proven_dead`, поэтому возвращается `LiveDoubleStart` без записи (`:449-453`).
- `claimMountAwaitingExpiry` (`Pool/CasServerRoot.cpp:493-566`) ждёт стабильности токена, но keeper первого диска продолжает продлевать аренду, токен меняется, и после `kMaxObservationRestarts` (=3, `:485`, проверка `:552-555`) возвращается тот же `LiveDoubleStart`.
- `mountWritable` на не-`Claimed`/не-`FencedSelf` результате бросает: `Pool/CasPool.cpp:697` — `throw Exception(ErrorCodes::ABORTED, "{}", mountDoubleStartMessage(srid, claim.body));` (текст в `Pool/CasServerRoot.cpp:456+`).
Так как `startup` публикует `cas_store` только в самом конце (комментарий `ContentAddressedMetadataStorage.cpp:791-796`), бросок оставляет диск незапущенным, а любой `store()`-класса вызов у него потом падает — писать в такой диск нельзя.
Порядок гонки при одновременном старте обоих дисков ничего не меняет: тот, кто проиграл, видит аренду соперника, не квалифицированную как fenced/clean/proven-dead, и так же отбивается; клеймить чужую живую аренду по одному лишь `expires_at_ms` код принципиально отказывается (`Pool/CasServerRoot.cpp:422-428`).
Поведение запинено гtest-ами: `src/Disks/tests/gtest_cas_mount.cpp`, `src/Disks/tests/gtest_cas_pool.cpp` (греп `DoubleStart`).

4) Пункт (b) — на всякий случай, если бы барьер (3) обошли. Настоящего «same-route» guard-а нет, но есть два места, релевантных сценарию:
- unique-ref guard в `Pool/CasPartWriteTxn.cpp:1160-1176`: при промоуте ref-а, который уже закоммичен на ДРУГОЙ манифест, без `allow_repoint` бросается `throwCasWriteRetryLater("promote: ref '...' already names a different committed manifest ... (unique-ref invariant; use republishRef for an intended repoint)")` (`:1171-1174`). То есть публикация «поверх» чужого/своего же коммита не молчаливая.
- `CachedPartFolderAccess::republishRef` (`Parts/PartFolderAccess.cpp:506-531`) действительно НЕ содержит проверки `src == dst`: при совпадающих `(ns, ref)` источник резолвится, назначение резолвится в тот же манифест, `entries` равны, и код уходит в ветку `:519-528` → `dropRef(src)` → `return true`. В одном диске такой вызов недостижим (разные каталоги кусков дают разные `ref`, `moveDirectory`/`renameParts` всегда с разными путями: `ContentAddressedTransaction.cpp:1255-1259`, `:1407`), а междисковый случай закрыт барьером (3). Тем не менее дешёвая защита «`src == dst` → бросить/no-op» была бы уместна как defense-in-depth — это единственное, что я бы оставил из находки, отсюда P3.

5) BACKLOG / история. Ни в `docs/superpowers/cas/BACKLOG.md`, ни в `BACKLOG/*.md` нет записи про два диска на одном пуле с одинаковым `server_root_id` (греп по `same pool`/`two disks`/`same server_root_id`/`MOVE PARTITION` даёт только несвязанные `[F4]`, `[killed-mid-move-partition-duplicate]`, `[B13]`). `git log -S"LiveDoubleStart"` показывает, что классификация живого двойного старта существует с самих коммитов Pool-слоя (`58d4f0b9ee0`, `c9834a0d8d2`, `a9ecbd215d3`, перенос `592b9b83568`), то есть «since fixed» коммита нет — это никогда не было дырой в том виде, в каком описано.

Итог: заявленная последовательность «конфиг принят → MOVE публикует и дропает тот же ref → кусок молча исчез» на HEAD недостижима, потому что второй диск вообще не монтируется. Класс DATA-LOSS снимается. Остаточное: (i) диагностика говорит «actively mounted by another LIVE server», хотя реальная причина — второй диск ЭТОГО же сервера с тем же `server_root_id`; стоит добавить в текст `mountDoubleStartMessage` подсказку про дубликат `server_root_id` в конфиге этого же сервера; (ii) отсутствие guard-а `src == dst` в `republishRef`.

## CAS-025 — Механика описана верно (rebuild стартует с пустых priors, свод edge-only, condemn-универсум сбрасывается, инкрементальный fold такие блобы больше не найдёт), но это осознанный fail-closed компромисс, уже зафиксированный в BACKLOG как «REBUILD R4 residual»: это удержание (retention), а не потеря, видимое как недренирующийся fsck `unaccounted`. (by-design, P3) {#cas-025}

Замечание про якоря: `CA/Gc/CasGc.cpp:2809-2824` и `:2876-2951` устарели. На HEAD это `src/.../ContentAddressed/Gc/CasGc.cpp:4104-4145` (нумерация генерации, `prior_runs`, `flush_shard`, `route_deltas`) и `:4180-4400` (обход владельцев, «A REBUILD CONDEMNS NOTHING», mint round, seal, `gc/state` CAS). Сама функция — `Gc::rebuildBaseline`, `CasGc.cpp:3849`.

(a) Пустые priors — подтверждено.
`CasGc.cpp:4110` — `std::vector<std::vector<RunRef>> prior_runs(gc_shards);` (пустые векторы), `:4132` — `prior_runs[shard] = std::move(out);` (итерируется только внутри самого rebuild, между его собственными attempt-ами). Свод — edge-only: `:4111-4128` вызывает `foldDeltasIntoGeneration(..., /*current_round*/0, /*condemn_round*/0, /*head_blob*/{}, /*peek_head*/{}, /*confirm_condemned_marker*/{}, /*out_retired*/nullptr, ...)`. Дельты идут только `+1`: `foldManifestEdges(id, +1, ...)` для committed refs (`:4203`), live precommits (`:4224`), и «unowned-but-alive» манифестов (`:4295`). Обнуление сводки: `:4356-4360` — `seal.condemned_summary[shard] = CondemnedSummary{};` для всех шардов, и `next.manifest_sweep_cursor = ""` + новый `snap_generation` (`:4370-4377`).
Следствие для блобов, чьё единственное свидетельство «не нужен» жило в condemn-универсуме (строки `kCondemned` ехали в прошлых runs): в перестроенной генерации у них нет строки. Не «потеряна пометка delete_pending» в смысле блокировки — а именно исчезла запись, по которой блоб был кандидатом на удаление.

(b) Крутить обратно из живых рёбер — нельзя, это подтверждается кодом. Свежая кондемнация в fold возможна только при переходе-в-ноль ПРИ наличии `head_blob`: `Gc/CasBlobInDegree.cpp:560` — `else if (cur_edges == 0 && cur_touched && head_blob)`. `cur_touched` требует дельту по этому блобу в текущем раунде, а дельты порождаются из ref-транзакций/манифестов — у блоба без владельца их больше не будет никогда. Прошлые строки могли бы приехать через `prior_runs`, но в перестроенной генерации их нет. Единственный, кто вообще перечисляет тела блобов, — fsck/inspect: `layout.blobsPrefix()` встречается только в `Tools/CasFsck.cpp:728` и `Tools/CasInspect.cpp:626-629`; в `Gc/` — ни разу. То есть да, это ПОСТОЯННОЕ удержание до внешнего вмешательства, а не самолечащееся состояние. Именно так это и задокументировано в коде: `CasGc.cpp:4304-4321` — «A REBUILD CONDEMNS NOTHING (spec §7)», с объяснением, что прежний LIST `blobs/` + condemn-всё-ненайденное был вектором ПОТЕРИ ДАННЫХ (r5-finding-4: обе ноги обхода listing-driven, одна врущая энумерация — и condemn прилетает живому блобу; наблюдённая форма `0x1430c`), и с прямым названием остатка: «a blob whose manifest no longer exists anywhere is unreclaimable ... It is retention, not loss, and it is bounded by that registry landing. NO substitute reclamation is added in its place».
Про «graduation guard is vacuous»: формально верно как форма кода — `Gc/CasBlobInDegree.cpp:488` — `if (e.marker_confirmed || !confirm_condemned_marker || confirm_condemned_marker(e))`, то есть отсутствующий callback пропускает всё. Но в rebuild эта ветка НЕДОСТИЖИМА: `RetiredEntry` появляются только из `prior_runs`, которые пусты, и `out_retired` = `nullptr`. Все НЕ-rebuild вызовы callback передают (`Gc/CasGc.cpp:3160`, `:3203`, через `Gc/CasGcShardPlan.cpp:57-61`), так что реальной дырки в guard-е нет — эту часть находки следует считать неверной по эффекту.

(c) Влияние двух рефакторингов на входы rebuild.
- retired-list→snapshot-run: строки condemned больше не живут отдельным объектом `RetiredSet`, они едут внутри runs/seal. Это ровно то, из-за чего «пустые priors» = «сброс condemn-универсума»; сам rebuild это учитывает и обязан выдать ТОТАЛЬНУЮ (по всем шардам) пустую сводку, иначе следующий обычный раунд упадёт fail-closed: `CasGc.cpp:4353-4360` («an ABSENT row and a zero row are different claims»). Семантику удержания рефакторинг не менял, только место хранения.
- ack-floor GC: в rebuild из этой линии участвует только `computeHeartbeatFloor` (`CasGc.cpp:4348-4350`) — liveness/fence-out мёртвых mount-ов, к восстановлению condemn-универсума отношения не имеет. Holds же наоборот едут через rebuild ВЕРБАТИМ (`:4249-4271`), а свежий hold для bodiless precommit минтуется тут же (`:4231-4247`, штамп round-а `:4331-4334`) — то есть входы rebuild ужесточаются, а не ослабляются.

(d) Операторская видимость и что реально осталось.
Такие блобы всплывают в fsck как `unaccounted` (`Tools/CasFsck.cpp:1016` — `default: ++report.unaccounted;`, вывод `:1160`), и этот счётчик после disaster-rebuild НЕ дренируется. Это впустую занятое хранилище до тех пор, пока не появится R4 (реестр build/upload, единственное, что может безопасно перечислить in-flight загрузки) или пока оператор не удалит их вручную по forensics. Плюс рядом лежит родственный документированный (обратный по знаку) остаток — «unowned-alive manifest edge over-protect» (`CasGc.cpp:4283-4293`).

BACKLOG / история. Запись существует и совпадает с находкой один-в-один: `docs/superpowers/cas/BACKLOG/gc.md:54` — **[REBUILD R4 residual — manifest-less blobs unreclaimable]** — «TRACKED, by design until R4 ... Such blobs are RETAINED and show as fsck `unaccounted` that does not drain after a disaster rebuild ... NOT a bug and explicitly NOT to be closed with a substitute reclamation: any rule that reclaims from an enumeration reintroduces the same vector. Closes when R4 lands.» Ближайший заголовок с анкором — `## GC correctness / observability follow-ups {#gc-followups}` (`gc.md:45`); у самого пункта персонального `{#...}` нет. Рядом: `gc.md:52` `[gc-rebuild follow-ups]` (over-protect leak, отсутствие gc-round-log строки для rebuild), `gc.md:60` `[REBUILD-SEAL-POINT-READ]`, `gc.md:142` `[gc-rebuild-lease-interlock]`.
Коммит, которым это состояние введено осознанно: `0cc71cece03` «ca: gc — REBUILD condemns nothing; fsck walks arithmetic streams (chain-broken fatal)» (`git log -S"A REBUILD CONDEMNS NOTHING"`). То есть это не забытая регрессия, а сознательная замена вектора потери данных на ограниченное удержание.

Итог: класс находки правильнее читать не как LEAK-баг, а как известный tracked-остаток. Понижаю до P3, pre-release не блокирует (утечка ёмкости после аварийного REBUILD, детектируемая fsck). Полезное, что можно взять из находки: (i) в BACKLOG-пункт стоит добавить персональный анкор; (ii) rebuild стоит явно логировать/эмитить оценку «сколько блобов осталось без строки» — сейчас оператор узнаёт об этом только запустив fsck (перекрывается с `gc.md:52`).

## CAS-020 — Механизм подтверждён — `getStorageObjects` теряет смещение payload и не имеет CA-гарда на стороне ИСТОЧНИКА, поэтому серверный copy-object (MOVE из CA / BACKUP на s3 того же хоста) копирует байты конверта; но «без ошибки» преувеличено: inline-файлы отдают ПУСТОЙ ключ, и операция целиком падает громко. (подтверждено, P2) {#cas-020}

## (a) Что реально возвращает `getStorageObjects` на HEAD

`ContentAddressedMetadataStorage::getStorageObjects`
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:1820-1867`):

- blob-файл: `const auto location = snap.pool->locate(*entry);` (`:1859`) и
  `return {StoredObject(location.key, path, location.length)};` (`:1865`). Смещение payload
  ОТБРОШЕНО осознанно, и это записано в комментарии `:1860-1864`: «StoredObject carries no range …
  the header offset is applied by `getBlobViewPlan`'s view window, the only byte-reading path».
- Смещение не нулевое и не выводимо из объекта: `Pool/CasManifestReader.cpp:144-160` —
  `BlobLocation{ .key = layout.blobKey(entry.ref), .offset = meta.blob_header_len, .length =
  entry.blob_size }`; `blob_header_len` — константа пула (для blob-пулов 256, валидируется в
  `Formats/CasPoolMetaFormat.cpp:38-46`). То есть payload НИКОГДА не начинается с offset 0.
- `StoredObject` вообще не имеет поля смещения (`Disks/DiskObjectStorage/ObjectStorages/StoredObject.h`),
  так что смещение не «потерялось по недосмотру» — оно НЕПРЕДСТАВИМО в возвращаемом типе.
- Inline-записи (мелкие файлы части, `INLINE_CAP == 1 MiB`, `ContentAddressedTransaction.cpp:98`,
  `:932-951`) отдают ПУСТОЙ ключ-заглушку: `:1828-1829`
  `if (auto bytes = tryGetInManifestBytes(path)) return {StoredObject("", path, bytes->size())};`
  (и `:1899-1900` в `getStorageObjectsIfExist`). Замысел заявлен в
  `ContentAddressedMetadataStorage.h:138-141`: «any consumer bypassing the prepareRead branch fails
  loudly, never reads wrong bytes». Для копирующих потребителей это работает как громкий отказ
  (пустой ключ → ошибка бэкенда), а НЕ как правильное копирование.
- Дополнительно: ключ не прогнан через `physicalKey()` (сравни `getBlobViewPlan` `:1987` и
  `readBlobPayload` `:2002`, где `physicalKey` применяется) — для emulated/local бэкенда ключ ещё и
  не физический.

Правильный путь чтения — единственный: `DiskObjectStorage::prepareRead` (`DiskObjectStorage.cpp:825-838`)
сначала пробует `prepareInManifestRead`, затем `getBlobViewPlan`, и только не-CA диски идут в
`metadata_storage->getStorageObjects(path)` (`:836-838`).

## (b) Потребители: где серверная копия реально достижима

`clonePart` (`src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:716-779`) ветвится ТОЛЬКО по
приёмнику: `if (dst_disk->isContentAddressed())` (`:735`) — тогда весь клон идёт через ОДНУ
CA-транзакцию побайтово (`copyDirectoryContentIntoTransaction`, `:685-713`, `readFile` →
`writeFile` → `copyData`). Это и есть «MOVE-to-CA fix» (L2). Для MOVE ИЗ CA (приёмник не CA)
ветки нет: `:760-775` вызывает `src_disk->copyDirectoryContent(...)`, `DiskObjectStorage`
`copyDirectoryContent` не переопределяет, значит `IDisk::copyDirectoryContent` →
`copyThroughBuffers` → `asyncCopy` → `from_disk.copyFile(...)` (`src/Disks/IDisk.cpp:157-205`) →
`DiskObjectStorage::copyFile` (`src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:291-323`).

Развилка там одна: `if (getDataSourceDescription() == to_disk.getDataSourceDescription())` (`:300`) →
серверная копия через `MultipleDisksObjectStorageTransaction::copyFile` →
`DiskObjectStorageTransaction::copyFileImpl`
(`src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:506-575`), где
`:522 const auto blobs_to_copy = src_metadata_storage->getStorageObjects(from_file_path);` и
`:551-552 copyObjectToAnotherObjectStorage(src_blob, dst_blob, ...)`. Иначе — безопасное чтение
через буферы (`IDisk::copyFile`, `src/Disks/IDisk.cpp:63-79`, то есть через CA-пайплайн чтения).

И развилка НЕ различает CA: `DataSourceDescription::operator==` (`src/Disks/DiskType.cpp:35-38`)
сравнивает `type, object_storage_type, description, is_encrypted, zookeeper_name` — и НЕ сравнивает
`metadata_type`, хотя `MetadataStorageType::CAS` существует (`src/Disks/DiskType.h:34`, поле `:46`) и
CA-хранилище его честно возвращает (`ContentAddressedMetadataStorage.h:230`). `description` для S3 —
это `uri.endpoint` (`Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h:70`), то есть только
схема+хост, без бакета и префикса. Итог: CA-диск на s3-эндпойнте E и обычный s3-диск на том же E
СРАВНИВАЮТСЯ РАВНЫМИ (`sameKind`, `DiskType.cpp:40-52`, ещё слабее). Гардов на стороне ИСТОЧНИКА нет
вовсе: grep по `copyFile|copyDirectoryContent|copyObject` в каталоге `ContentAddressed/` — ноль
попаданий.

Что при этом копируется физически: `S3ObjectStorage::copyObjectToAnotherObjectStorage`
(`Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:692-720`) берёт
`size = S3::getObjectSize(...)` ИСХОДНОГО объекта (`:705`) и делает `copyS3File(..., src_offset=0,
src_size=size, ...)` (`:711-717`) — то есть ВЕСЬ объект вместе с конвертом, а метаданные приёмника
записываются с размером payload (`copyFileImpl:523-525`, `from.bytes_size`). Прочитанные с приёмника
первые `payload_len` байт = заголовок конверта + payload без хвоста. Это порча, и на уровне
отдельного файла — молчаливая.

Кто доходит до `clonePart` (значит наследует эту развилку): `ALTER … MOVE PART|PARTITION TO
DISK|VOLUME` (`MergeTreeData.cpp:6947`, `:7030`, диспетчер `:7151-7155`) → `movePartsToSpace`
(`:10253`) → `moveParts` (`:10334`) → `MergeTreePartsMover::clonePart` (`:10405`/`:10438`,
`MergeTreePartsMover.cpp:275`/`:281`), TTL/фоновые перемещения через `scheduleDataMovingJob`
(`:10224`) → `selectPartsForMove` (`MergeTreePartsMover.cpp:103`) → тот же `movePartsToSpace`, и
`IMergeTreeDataPart::makeCloneOnDisk` (`IMergeTreeDataPart.cpp:2474`). `MOVE_PARTITION` явно
разрешён CA-гардом `MergeTreeData.cpp:6729/6786` (комментарий `:6773-6776` прямо говорит, что
кросс-дисковый `MOVE … TO DISK/VOLUME` идёт байтовым `clonePart`-путём — что верно только при
НЕравных `DataSourceDescription`).

BACKUP — тот же класс, и там развилка ещё слабее:
- `BackupWriterS3::copyFileFromDisk` (`src/Backups/BackupIO_S3.cpp:382-423`): `sameKind` (`:387-388`),
  затем `src_disk->getBlobPath(src_path)` (`:391-392`) — а это буквально
  `getStorageObjects` (`DiskObjectStorage.cpp:948-959`) — и `copyS3File(..., start_pos, length, ...)`
  (`:394-403`) без `blob_header_len`. ДОСТИЖИМО и портит.
- `BackupWriterAzureBlobStorage::copyFileFromDisk` (`src/Backups/BackupIO_AzureBlobStorage.cpp:161-193`):
  предикат вообще только по типу (`:167-168`) — слабее всех.
- `BackupWriterDisk::copyFileFromDisk` (`src/Backups/BackupIO_Disk.cpp:129-152`) → `src_disk->copyFile`
  (`:145`) → та же развилка `:300`.
- `BackupWriterFile` (`BackupIO_File.cpp:140-172`) требует `getBlobPath(...).size() == 1` и
  `length == fs::file_size(...)` (`:152`, `:158`) — на CA размеры не совпадают (payload vs
  payload+заголовок) и ключ не физический → откат на буферный путь; не портит, но и не гард.
- `BackupWriterDefault::copyFileFromDisk` (`BackupIO_Default.cpp:79-93`) — безопасен (чтение через диск).
- Контрольные суммы по удалённому пути ОТКАЗАНЫ заранее: `canCalculateChecksumFromRemotePath` →
  `areBlobPathsRandom()` (`BackupEntryWithChecksumCalculation.cpp:124-127`), а CA возвращает `false`
  (`ContentAddressedMetadataStorage.h:263`), так что `calculateChecksumFromRemotePath` (`:272-334`,
  `getStorageObjects` на `:298`) не вызывается.
- RESTORE В CA отказан громко: `writeFileUsingBlobWritingFunction` →
  `generateObjectKeyForPath` → `notYet` (`ContentAddressedTransaction.cpp:530-532` через
  `DiskObjectStorageTransaction.cpp:394`); штатный путь — целочастная транзакция
  `MergeTreeData.cpp:7543-7545`.

## (c) Есть ли явный отказ для MOVE ИЗ CA

Нет. Существующие CA-отказы закрывают другое: BACKUP через временные хардлинки
(`DataPartStorageOnDiskBase.cpp:422-427`, `SUPPORT_IS_DISABLED`; и там же в комментарии `:420-421`
прямо сказано, что второй путь «uses getStorageObjects and round-trips on a CAS disk, so it is left
untouched» — именно это предположение и неверно при равном `DataSourceDescription`), CA как ПРИЁМНИК
(`generateObjectKeyForPath`/`truncateFile`/автокоммит в `ContentAddressedTransaction.cpp:530-532`,
`:757`, `:770-771`, `:1655-1656`), репликация (`DataPartsExchange.cpp:159-164`, `:404-405`).
Комментарий `DiskObjectStorageTransaction.cpp:565-567` («Unreachable on CA») верен ТОЛЬКО для CA-приёмника:
`getStorageObjects` источника читается на `:522`, ДО `generateObjectKeyForPath` приёмника.

## (d) Громко или тихо

Здесь заявление аудита («corrupt destination part with no error») преувеличено, и это существенно:

- каждый blob-файл копируется молча неверно (см. выше);
- НО в любой реальной части есть файлы <= `INLINE_CAP` (1 MiB): `count.txt`, `columns.txt`,
  `checksums.txt`, `metadata_version.txt` и т.п. — они inline (`ContentAddressedTransaction.cpp:932-951`),
  и `getStorageObjects` отдаёт для них ПУСТОЙ ключ (`:1828-1829`). `copyObjectToAnotherObjectStorage`
  на пустом ключе сразу делает `S3::getObjectSize` (`S3ObjectStorage.cpp:705`) → ошибка бэкенда →
  исключение;
- в `clonePart` это исключение ловится и приёмник вычищается (`DataPartStorageOnDiskBase.cpp:766-772`,
  `removeRecursive`), в BACKUP — падает вся операция.

То есть на уровне ЧАСТИ/операции исход — громкий отказ с невнятным сообщением (про пустой/отсутствующий
ключ), плюс мусорные объекты в приёмнике от уже успевших скопироваться blob-ов. Молчаливая порча
достижима только для потребителя, копирующего исключительно blob-файлы; такого потребителя в дереве я
не нашёл (в MOVE и BACKUP части всегда идут целиком). Поэтому класс — реальный незакрытый
integrity-гард с гарантированным «спасательным» громким отказом, а не тихая порча данных: P2, не
pre-release-блокер.

## История / BACKLOG

Отдельного пункта под это НЕТ. `git log -S "since fixed"` по CAS-коду ничего не даёт; grep по
`BACKLOG.md` и `BACKLOG/*.md` на «envelope offset», «CAS-020», «CAS-047», «StoredObject carries no
range» — ноль. Ближайшее по теме — `BACKLOG/replication.md`
`[move-part-to-ca-architecturally-unimplemented]` (про MOVE В CA; его «architecturally
unimplemented» шапка объявлена УСТАРЕВШЕЙ в `BACKLOG.md:665-670`, оба слоя приземлились), и там же
явно сказано «unaffected: … off-CA moves (CA→local)» — что верно ровно для НЕравных
`DataSourceDescription` (CA-s3 → local) и НЕ верно для CA-s3 → plain-s3 на том же эндпойнте. Родня
того же семейства — `{#issue-2173-freezeremote-gap}` (`BACKLOG.md:635-670`): там пропущенная
CA-ветка в `freezeRemote`, здесь — пропущенный CA-гард на стороне источника серверной копии.

Минимальная структурная починка (напрашивается, дешёвая): либо включить `metadata_type` в
`DataSourceDescription::operator==`/`sameKind` (`src/Disks/DiskType.cpp:35-52`), либо добавить
CA-гард источника в `DiskObjectStorage::copyFile:300` и в `DiskObjectStorage::getBlobPath:948`
(последнее закрывает и оба BACKUP-writer'а сразу).

## CAS-021 — Все шесть цитат про контроллер на HEAD текстуально верны, но каждое опасное следствие уже нейтрализовано; остаточный дефект — устаревшая in-process памятка condemn-маркера, чинить её пере-чтениями пользователь отказался (принятый остаток + переименование в наблюдаемости). (частично, P3) {#cas-021}

## Что уже адъюдицировано (не переоткрываем)

Находка уже разобрана 2026-08-20 как issue https://github.com/Altinity/ClickHouse/issues/2207;
вердикт записан в `docs/superpowers/cas/BACKLOG.md:502-540` (секция
`## CAS-021 (issue #2207) adjudication follow-ups ... {#cas-021-followups}`). Вердикт тот же:
поведение контроллера описано аудитом правильно, но ни одно из заявленных нарушений целостности
на текущем дереве не достижимо. Ниже — перепроверка на HEAD (`684161dcc03` + docs-коммиты;
`git log -S "since fixed"` по CAS-коду ничего не даёт).

## Цитаты аудита на HEAD (все подтверждены текстуально)

- Равенство байт трактуется как доказательство собственного авторства (create-путь):
  `Backend/CasRequestControl.cpp:290-296` — `if (got->bytes == expected_bytes) ... return
  CasWriteOutcome::Committed; /// identical deterministic bytes -> the earlier attempt DID commit`.
  Разные байты по тому же ключу — не «неопределённость», а `CORRUPTED_DATA` (`:298-300`),
  то есть fail-closed.
- То же на overwrite-пути: `Backend/CasRequestControl.cpp:561-569` —
  `else if (got && got->bytes == bytes_s) { ... return {CasOverwriteOutcome::Committed, got->token}; }`,
  и рядом честный комментарий про природу неопределённости `:544-546` («PreconditionFailed alone
  does NOT prove a real conflict — it may be our own earlier attempt's write landing»).
- Неопределённость, отражённая как чужая занятость: `Backend/CasRequestControl.cpp:570-574`
  (`else if (got) ... Conflict`), `:466`, `:495` (`Occupied`); метка `NotUnresolved`
  (`:329`, `:707`) действительно вводит в заблуждение.
- `NoSuchKey` → `PreconditionFailed`: `Backend/CasObjectStorageBackend.cpp:156-159`
  (`e.isPreconditionFailed() || e.getExceptionName() == "NoSuchKey" || e.getS3ErrorCode() ==
  Aws::S3::S3Errors::NO_SUCH_KEY` → `PutOutcome::PreconditionFailed`). Направление отображения
  fail-safe и это заявлено в комментарии `:141-142`: «a misread error becomes a retryable
  PreconditionFailed/Conflict, never a false success».

## Почему опасные следствия не достигаются на HEAD

- Удаление защищено нормативным пере-чтением in-degree на самом delete-сайте:
  `Gc/CasBlobInDegree.cpp:423-432` («THE DELETE-SITE IN-DEGREE RE-READ IS NORMATIVE (spec §5, third
  arm). It is not an optimization and not defense-in-depth»).
- Удаление — строго exact-token, а воскрешение ротирует `incarnation_tag`, поэтому ETag
  воскрешённого тела гарантированно отличается от осуждённого:
  `Pool/CasPartWriteTxn.cpp:710-719` («`buildHeader` mints a FRESH `incarnation_tag` ... so the
  resurrected body (and hence its ETag) differs from the condemned incarnation regardless of
  edge-before-observe»), сама запись `:731` и `:761` (`backend().resurrect(...)`), плюс fence-чек
  `store->checkFenceOrThrow(displace_admitted_generation)` (`:723`, `:753`).
- Etag, «разрешённый по равенству», не потребляется никем: `Gc/CasGc.cpp:127-137`
  `writeCondemnedMeta` возвращает только `bool` (`... .outcome == CasOverwriteOutcome::Committed`),
  токен/etag из результата CAS не читается.
- Авторство в ref-лейне решается побайтовым сравнением payload, который несёт идентичность
  транзакции: `Pool/CasRefLedger.cpp:165` (`classifyRefLogOccupant`), вызовы `:909`, `:2244`, `:3597`.
- Гейт GC — «существует durable-свидетельство Condemned», а GET на разрешении равенства именно это
  и доказывает; чужой Condemned-маркер удовлетворяет предикат by design, ровно как уже-Condemned
  ветка (`Gc/CasGc.cpp:137` `return true;` на уже осуждённой мете).

## Что реально осталось (остаток стоит на HEAD)

Устаревшая мемоизация condemn-маркера. In-process реестр —
`Gc/CasGc.h:968` `std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;`, заполняется в
`Gc/CasGc.cpp:442-446` (`noteCondemnMarkerDurable`), читается в `:449-452`
(`condemnMarkerConfirmedInProcess`), забывается только в `:454-458` (`forgetCondemnMarker`) и вызовы
этого забывания стоят ИСКЛЮЧИТЕЛЬНО на fold-путях, где запись покидает конвейер:
`Gc/CasGc.cpp:870` (redelete-выход), `:914` (spared), `:963` (replaced/superseded).

Единственный законный переход `Condemned -> Clean` делает ПИСАТЕЛЬ —
`Pool/CasPartWriteTxn.cpp:537` `writeResurrectMetaClean`, вызовы `:567`, `:734`, `:762` — и он
физически не может инвалидировать приватный in-process set у `Gc` (другой класс, другой объект,
никаких вызовов `forgetCondemnMarker` вне `Gc`; проверено grep'ом по всему каталогу CAS). Поэтому
гейт градуации `Gc/CasGc.cpp:1885-1889` (`if (condemnMarkerConfirmedInProcess(entry.ref,
entry.token)) return true;`) может пропустить запись, чья durable-мета уже `Clean`, — без единого
запроса, по памятке. Приоритет остаточной ветки на градуации: памятка проверяется ПЕРЕД
`loadMeta`-пере-чтением (`:1890-1896`), так что пере-чтение её не спасает.

Последствие, когда это срабатывает (сверх-редкая гонка «воскрешение без промежуточного fold»): ОДИН
лишний `deleteExact` — то есть один S3 DELETE, бесплатный, и он самозалечивается: воскрешение
ротировало токен, значит `deleteExact` (`Gc/CasGc.cpp:802`) даёт `TokenMismatch` (дизамбигуация
412-на-отсутствующем — `:814-823`), исход классифицируется как `Replaced` (`:825-829`), мета НЕ
трогается (условие `:864` только `Deleted`/`Absent`, и явный комментарий `:859-863`: удалять мету
на `Replaced` нельзя, писатель сам вернул её в `Clean`), а памятка тут же сбрасывается
(`:870` `forgetCondemnMarker`). Данные не теряются:
удаляется старая инкарнация по её собственному токену, живого тела этот DELETE не касается.

Починка пере-чтением ОТКЛОНЕНА пользователем 2026-08-20 (зафиксировано в
`BACKLOG.md` {#cas-021-followups}, пункт (2)): она стоила бы +1 БИЛЛИНГОВЫЙ GET на каждую
градуирующую осуждённую запись на ОБЩЕМ пути (класс бюджета GET из P9), чтобы сэкономить бесплатные
DELETE в редкой гонке — лечение хуже болезни; бесплатной инвалидации не существует, окно по
определению «никто не наблюдал воскрешение». Не переоткрываю.

Санкционированные остатки работы (оба — не про поведение): (1) «honesty patch» над контроллером
(вынести исход, разрешённый по равенству, из `Committed` — например `IntendedStateDurable`;
перестать возвращать наблюдаемый токен занявшего на этой ветке — сегодня он всё равно никем не
читается; переименовать метку `NotUnresolved`; добавить doc-блок про модель доверия и таблицу
разрешимости владения по классу ключа) и (2) метка наблюдаемости на самозалечивающем сайте
`Gc/CasGc.cpp:814-830` — считать/логировать это как «spared by token rotation», а не как аномалию.
Оба — ноль дополнительных запросов и ноль изменений durable-операций, поэтому P3 и не pre-release.

## CAS-016 — Оба факта верны буквально — `attempt_timeout_ms` действительно не уходит на провод, а чтение payload идёт обычным путём объектного хранилища, — но и то и другое задокументированный дизайн; «нет таймаута попытки, нет лимита попыток, нет классификации» ложно, реальный остаток — отсутствие startup-сверки бюджета с `request_timeout_ms` диска и уже отслеживаемые в бэклоге небюджетированные read/HEAD/LIST. (частично, P3) {#cas-016}

**(a) `attempt_timeout_ms` на проводе — факт подтверждён, но это заявленный контракт, а не забытая проводка.**
Все продакшн-использования поля действительно арифметические: `if (now_ms() + budget.attempt_timeout_ms > deadline_ms)` перед каждой попыткой (`Backend/CasRequestControl.cpp:339`, и то же на `:262,434,515,597,682`) плюс startup-неравенство против TTL маунт-лизы (`:134-140,151-157`). Однако сам заголовок ровно это и объявляет: «`CasRequestController` uses this ONLY as a per-attempt scheduling check … the actual socket-level wait is configured on the object storage's client (the object storage backend's single-attempt client), not by this struct» (`Backend/CasRequestControl.h:147-151`). То есть утверждение находки «never reaches the wire» верно как факт и ложно как дефект — это документированное разделение ролей.

Проводка на провод существует и она другая: CAS-бэкенд просит у хранилища профиль повторов, а не таймаут. `ws.object_storage_retry_profile = ObjectStorageRetryProfile::SingleAttempt` (`Backend/CasObjectStorageBackend.cpp:835`), причём бэкенд, не умеющий этот профиль, отвергается на открытии writable-пула (`:102-103`, ср. `CasBackend.h:285`). На стороне S3 профиль резолвится в отдельный клон клиента: `used_client = getSingleAttemptClient()` (`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:363`), где `cfg.retry_strategy.max_retries = 0` и `cfg.retryStrategy = std::make_shared<S3::SingleAttemptRetryStrategy>()` (`:970-971`, стратегия — `src/IO/S3/Client.h:360-368`, `GetMaxAttempts() == 1`). Так что для условных записей ref-лейна прозрачных SDK-ретраев нет вовсе — «500-retry profile» к этому пути не относится.

Чего действительно нет: `getSingleAttemptClient` НЕ переопределяет `requestTimeoutMs` — сокетный таймаут остаётся тем, что задан в конфиге диска (дефолт `DEFAULT_REQUEST_TIMEOUT_MS = 30000`, `src/IO/S3Defines.h:10`), при дефолтном `attempt_timeout_ms = 5000` (`CasRequestControl.h:151`). Это единственная содержательная часть находки: премиса startup-неравенства `attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms` (`CasRequestControl.cpp:134-140`, при дефолтных 5000+2000 < 30000 = `Pool/CasPool.h:182`) не проверяется end-to-end — одна попытка может занять на проводе 30 s, то есть ровно TTL лизы, и «влезет ли попытка в дедлайн» становится оценкой снизу. Последствия при этом fail-closed, а не потеря данных: после каждой попытки проверяется fence, и потеря fence поверх доказанно/возможно севшей записи даёт `CasUnresolvedReason::FenceLostPostWrite` + `CASConditionalWriteFenceLostPostWrite` (`CasRequestControl.cpp:412-413,472,538,566,619,647`), то есть исход «не разрешён», а не «считаем закоммиченным». Разумный остаток — startup-проверка соотношения бюджета и `request_timeout_ms` диска (та же группа, что подпункт (c) в бэклоге).

Кроме того, «no attempt cap» прямо ложно: `max_attempts = 16` (`CasRequestControl.h:173`), цикл `for (uint32_t attempt = 1; attempt <= budget.max_attempts; ++attempt)` (`CasRequestControl.cpp:331`), и «no operation deadline» ложно: `operation_deadline_ms = 90000` (`:168`) — именно он проверяется в цитируемой находкой арифметике.

**(b) Чтение payload — подтверждено, но это осознанный дизайн, и админ-гейт на месте.**
Байты действительно читаются обычным `readObject`: `ContentAddressedMetadataStorage.cpp:2001` (внутри `readBlobPayload`, `:1995-2004`) для read-your-writes транзакции, а committed-чтение вообще не идёт через метаданные CAS — `getBlobViewPlan` возвращает физический объект и окно, а дальше работает штатный конвейер диска (`src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:832`, `:875-881`). Причина названа в самом коде: «rides the STANDARD pipeline below (gather/caches/async prefetch — same chain as plain object-storage disks, so right-mark bounds reach the object reader and its range requests stay drainable, B116)» (`DiskObjectStorage.cpp:819-823`). Это результат работы по производительности чтения (B113/B116): протаскивать payload через контролируемые операции бэкенда означало бы потерять file cache, page cache, distributed cache и асинхронный префетч.

Утверждение «no CAS classification» неверно в части допуска: оба входа гейтятся классом операций CAS — `checkOpAdmitted(CasOpClass::ContentRead)` в `getBlobViewPlan` (`ContentAddressedMetadataStorage.cpp:1963`) и в `readBlobPayload` (`:2000`), так что на Vanished/IdentityLost диске чтение падает типизированной ошибкой, а не сырым «no such key». Верно лишь то, что сам GET не учитывается request-контроллером и не даёт CAS-события; наблюдаемость при этом не нулевая — это обычные `S3GetObject`/`blob_storage_log` объектного слоя.

**(c) Практическое последствие — «медленно и с ошибкой», а не вечный хэнг.**
Для payload-чтений действует дефолтный профиль: `DEFAULT_RETRY_ATTEMPTS = 500` (`src/IO/S3Defines.h:43`) c backoff 25 ms → cap 5 s (`:44-45`) и `DEFAULT_REQUEST_TIMEOUT_MS = 30000` (`:10`), плюс `s3_max_single_read_retries` (дефолт 4, `:33`) уровнем выше. То есть каждая HTTP-попытка ограничена 30 s, зависание не бесконечно, но худший случай при «5xx-петле» действительно длинный (сотни попыток). Два реальных ограничителя: `ShouldRetry` немедленно прекращает ретраи при отмене запроса — `if (CurrentThread::isInitialized() && CurrentThread::get().isQueryCanceled()) return false;` (`src/IO/S3/Client.cpp:119-120`), поэтому `max_execution_time`/отключение клиента разрывают цикл; и 412 больше не считается retryable (`:112-114`). `http_receive_timeout` тут не при чём — это HTTP-интерфейс сервера, не S3-клиент. Не покрыты отменой фоновые задачи (мержи, фетчи): для них худший случай — длинная серия ретраев до ошибки. Ключевое: это НЕ CAS-специфика — ровно тот же профиль обслуживает любое чтение MergeTree с S3-диска, поэтому класс LIVENESS для CAS завышен.

**BACKLOG / история.** Прямо покрыто пунктом «[timeout-retry RFC residuals]», подпункт (c): «bounded read/HEAD/LIST retries + startup validation for the non-ref plain-object paths (`casPutObject`/`casRemoveObject` still use the disk's default retry policy)» (`docs/superpowers/cas/BACKLOG/ref-protocol.md:20`, секция `{#ref-protocol-rev6}`). Смежная уже вынесенная находка — CAS-011 (`docs/superpowers/cas/2031-triage.md:306`), где тот же residual разобран для `CasPlainObjects`; CAS-016 — не дубликат (там плоские объекты записи, здесь бюджет на проводе и путь чтения payload), но закрывается тем же пунктом бэклога. Профиль SingleAttempt и его клиент появились коммитами `a5783037dbb` («generic ObjectStorageRetryProfile + S3 single-attempt client owned by S3ObjectStorage (F5a)») и `58ac1af18c6` (фикс ABA на кэше клиента).

**Что реально осталось.** (1) Нет startup-сверки `attempt_timeout_ms` с фактическим `request_timeout_ms` клиента диска — премиса lease-неравенства не проверяется, хотя нарушение fail-closed. (2) Read/HEAD/LIST и плоские объекты по-прежнему на дефолтной политике диска, без операционного дедлайна и разрешения неопределённых исходов — записанный residual. Ни то, ни другое не блокирует релиз: корректность держится на fence-проверках и exact-token условности, а «медленно вместо ошибки» — общее свойство S3-дисков ClickHouse.

## CAS-017 — Порядок «защёлка до durable» и терминальность `Closed`/`Faulted` подтверждены, но «пустой catch глотает reopen», «нет выхода из `Removing`» и «постоянно нерабочая таблица» на HEAD ложны — есть fail-close-разрешение с закреплённым тестом, GC-реклейм `Removing` и полностью in-memory защёлка; реальный остаток — read-путь отвечает «нет ref» вместо retry-later в окне защёлки и одна ветка `Faulted` без автоматического remount. (частично, P3) {#cas-017}

**(a) Последовательность удаления — факт подтверждён, включая мотив.**
`dropNamespace` действительно закрывает локальный положительный лейн ДО публикации `Removing`: `rt->removal_admission_closed = true;` (`Pool/CasRefLedger.cpp:4935`) и сразу же ожидание слива очереди `rt->cv.wait(queue_lock, [&]{ return !rt->leader_active && rt->pending.empty(); })` (`:4936-4940`); durable-переход `Live → Removing` через `CasRefCatalog::beginRemoving` идёт только после этого (`:4974-4977`). Порядок объяснён на месте: «Close the local positive lane BEFORE publishing `Removing`. Calls already admitted ahead of this point drain first…» (`:4929-4932`) — иначе уже допущенный положительный append сел бы в лог после публикации `Removing` и терминальный fold считал бы его.

Что происходит при throw между защёлкой и durable — противоположно описанию находки. Есть явный разрешающий обработчик (`:4998-5031`): свежее чтение каталога; `Removing` под той же инкарнацией = успех (`:5010-5016`); ТОЧНО тот же `Live`-ряд плюс живой fence = переоткрытие защёлки `rt->removal_admission_closed = false; rt->cv.notify_all();` (`:5019-5022`); любой иной случай остаётся закрытым — «fail-close» — и исходная ошибка уезжает наверх (`:5030-5031`). Пустой `catch` на `:5025-5029` глотает НЕ reopen, а неудачу самой попытки разрешения, и его комментарий это и заявляет: «The original failure remains the caller-visible one. Failure to prove an exact fresh `Live` row deliberately leaves admission closed». Ровно этот сценарий закреплён тестом `CASRefWriterNamespaceRemoval.PredurableCatalogReadFailureReopensExactLiveLane` (`src/Disks/tests/gtest_cas_ref_writer.cpp:4090-4110`): сбой пост-защёлочного чтения каталога → `dropNamespace` бросает, каталожный ряд остаётся исходным `Live`, и следующая ПОЛОЖИТЕЛЬНАЯ мутация проходит без ошибки. Второй выход закреплён отдельно: `RemovalAppendFailureLeavesRemovingAndRetryCompletes` (`:4148`) — повтор `DROP` доводит удаление до конца.

Есть и вторая, независимая точка защёлки, и она durable-обоснована: в `commitRefChunk` положительный append, увидевший точный ряд не в состоянии `Live`, латчит `removal_admission_closed = true` (`Pool/CasRefLedger.cpp:3265-3271`) — «A non-`Live` exact row permanently closes this local positive lane».

**(b) Состояния ref-лейна и их выходы.**
Полный набор — `Ready, Writing, Wedged, NeedsRecovery, Closed, Faulted` (`Pool/CasRefLedger.h:44-52`). Терминальны два:
- `Closed` — эпоха закрыта seal'ом преемника (`:2407` в разрешении wedge и `:3645` в append). Смысл — «этот писатель низложен»: «This mount's append lane resumes only under a later epoch» (`:3646-3650`). Выход автоматический и не через этот лейн: низложение означает, что лизу забрал преемник, а superseded/foreign renewal латчит fence и планирует remount — `tripMountLost(); scheduleRemount();` в fence-колбэках keeper'а (`Pool/CasMountRuntime.cpp:247-254`); remount выбрасывает весь `RefTableRuntime`.
- `Faulted` — «невозможная» интерференция или контрактное нарушение (`:2413`, `:3315`, `:3623`, `:3671`). Две из четырёх веток прямо гонят реакцию `on_impossible_interference` (`:2524`, `:3317`, `:3673`), а она в `Pool` делает `mount_runtime.tripMountLost(); mount_runtime.scheduleRemount();` (`Pool/CasPool.cpp:1471-1472`). Код прямо отвечает на претензию находки: «Failing closed is right, but failing closed FOREVER is not: without this the mount stays blocked on this table until somebody notices and remounts by hand» (`Pool/CasRefLedger.cpp:3663-3666`).

Реальный остаток здесь один: ветка «оккупант нечитаем» (`:3619-3624`) ставит `Faulted` БЕЗ `on_impossible_interference`, полагаясь на то, что следующий flush пере-сообщит через `resolveWedgeOnce` (`:3320-3326`). Но `resolveWedgeOnce` на не-`Wedged` лейне возвращает `StillWedged` с `INVALID_STATE` (`:2155-2185`), а `flushRefBatch` на этом исходе завершает всю очередь и выходит (`:2701-2705`), никогда не доходя до гейта `:3310`, который единственный вызывает реакцию. То есть эта ветка действительно даёт per-namespace лейн без автоматического выхода — до remount/рестарта. Достижима она только при уже «невозможном» состоянии (чужой объект по нашему id) плюс сбое GET этого объекта, поэтому это P3, а не блокер.
Побочно: пункт бэклога `{#lane-terminal-reported-as-retryable}` (`BACKLOG/ref-protocol.md:26`) на HEAD УЖЕ ЗАКРЫТ — арм `:3310-3331` рапортует `CORRUPTED_DATA`, а не retry-later; исправлено коммитом `21617aedda2` («ca: ref — format bump B …»), запись в бэклоге устарела.

**(c) «Постоянно нерабочая таблица» — опровергнуто, в том числе без рестарта.**
Защёлка и состояние лейна — поля in-memory `RefTableRuntime` (`Pool/CasRefLedger.h:836-842`), принадлежащего `CasRefLedger` данного маунта; рестарт, `SYSTEM CAS UNMOUNT/MOUNT` и self-remount очищают и то и другое по построению. Каталожный ряд в `Removing` тоже не тупик: его удаляет GC-раунд, после чего имя рождается заново — это отдельно исследовано и закреплено в бэклоге как `{#cas-join-set-truncate}` («**Verdict: TRANSIENT, not permanent** … After draining GC (two rounds …), the identical call mints a fresh incarnation and writes succeed normally — self-healing, no operator action required», `docs/superpowers/cas/BACKLOG.md:148-166`) с тестом `FilesOnlyNamespaceTruncateThrowsRetryLaterUntilGcReclaimsThenRebirths` (`gtest_cas_ref_writer.cpp:4582`). Создание против `Removing` бросает типизированный retry-later, а не отказывает навсегда (`Pool/CasRefLedger.cpp:4700-4710`, `:4728-4731`, `:1252-1258`), и `CreateAgainstRemovingRetriesWithoutMutation` (`gtest_cas_ref_writer.cpp:4646`) закрепляет, что при этом не тратится ни одна мутация.

**Что реально осталось (и это самая содержательная часть находки).** Асимметрия «запись vs чтение» в окне защёлки. Положительная запись получает ЧЕСТНЫЙ типизированный retry-later («CAS namespace '{}' is Removing: positive ref mutation admission is closed…», `Pool/CasRefLedger.cpp:1943-1947`, `:1977-1981`), а чтение молча получает «ничего нет»: `acquireReadableRefTableRuntime` при защёлке возвращает `nullptr` (`:565-571`), из-за чего `resolveRef` даёт `std::nullopt` (`:283-288`), `listRefs` — пустую карту (`:355-364`), `hasAnyRefWithPrefix` — `false` (`:385-388`), `namespaceFilesLifeIfReadable` — `nullopt` (`:4745-4749`). Пока каталог всё ещё `Live`, это сфабрикованное отсутствие. Окно не микроскопическое: `cv.wait` на `:4936` не имеет таймаута и ждёт слива уже идущего лидера, который сам ограничен только бюджетами контроллера (`operation_deadline_ms = 90000`, а recovery-бюджет — 120000, `Backend/CasRequestControl.h:168,195`). Смягчающие обстоятельства, из-за которых это не P1/P2: (i) окно существует только пока в ЭТОМ же процессе исполняется `dropNamespace` именно этого namespace, а он вызывается из `removeRecursive`/`moveDirectory`/unfreeze (`ContentAddressedTransaction.cpp:1058,1083,1091,1275`) под эксклюзивной блокировкой таблицы; (ii) probe присутствия намеренно НЕ верит защёлке, а ревалидируется холодным путём (`Pool/CasRefLedger.cpp:4773-4790` и далее — «A missing row is the one answer that must never be manufactured by a race»), и `namespaceLife` при защёлке сначала делает `reconcileCatalogCut`, а потом бросает retry-later (`:4696-4712`); (iii) последствие чтения — ошибка «файла нет»/пустой список, а не разрушительное действие. Тем не менее «отвечать absent там, где запись отвечает retry-later» — ровно тот класс, который в этом же файле объявлен запрещённым для probe; правильная правка — вернуть из read-пути типизированный retry-later (или ревалидировать каталог, как probe), а не `nullptr`. Плюс упомянутая ветка `Faulted` без `scheduleRemount`. Оба пункта — кандидаты в `{#ref-protocol-ledger}`, оба P3.

## CAS-022 — Ветка «нет строки в каталоге» в постраничном планировщике sweep-а действительно не проверяет ни watermark, ни coverage, ни §6-предпосылку, и её обоснование («строка каталога публикуется раньше любого объекта жизни») на HEAD неверно — тело манифеста пишется до создания строки; но окно узкое, а последствие громкое и не data-loss. (частично, P2) {#cas-022}

## Стале-анкеры

Все номера строк в findings (`CA/Gc/CasOrphanManifestSweep.cpp:546`, `:547-561`, `:605-636`, `:653`, `:660-682`, `:684-716`) устарели. Файл на HEAD: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp` (915 строк). Релевантные места: `sweepNamespace` — строки 494-598; постраничный планировщик `planManifestCursorPage` — 601-912; ветка «нет строки каталога» — 817-895.

## (a) Что делает sweep при отсутствии строки каталога

Есть ДВА пути удаления, и ведут они себя по-разному.

1. `sweepNamespace` (адресный, по одному build-префиксу; вызывается из decommission/tools). Здесь гейты стоят ПЕРЕД удалением и строка каталога обязательна:
   - `CasOrphanManifestSweep.cpp:499-500` — `if (!prefixEligible(store, ns, prefix)) return 0; /// not eligible by the durable watermark fact — delete nothing`;
   - `prefixEligible` (`:475-490`) берёт авторитет только из durable mount-lease floor: `:481-482` `const auto floor = floorForNamespace(store, ns); if (!floor) return false;` — «отсутствующий floor значит НЕ eligible»;
   - `:518-520` — `const CatalogEntry * catalog_entry = catalogEntryOf(catalog_cut, ns); if (!catalog_entry || catalog_entry->state == NsState::Creating) return 0; /// absent/Creating names have no recovery authority and therefore no deletion authority`.
   Т.е. для этого пути утверждение findings ЛОЖНО: без строки каталога не удаляется ничего.

2. `planManifestCursorPage` (периодический раунд GC, тот, что реально крутится). Здесь ВСЕ защиты условны по `catalog_entry`:
   - watermark: `:700-711` — блок `if (catalog_entry) { ... prefixEligible ... }`, т.е. при `catalog_entry == nullptr` `prefixEligible` не вызывается вообще;
   - авторизация восстановления каталога: `:740` `if (catalog_entry && !catalog_recovery_authoritative)`;
   - `Creating`: `:751` `if (catalog_entry && catalog_entry->state == NsState::Creating)` (заметьте: `catalogEntryOf`, `:132-137`, возвращает и `Creating`-строки, так что «Creating» уходит в retain, а не в эту ветку);
   - protection view / `active`: `:759-800` и `:817` `if (catalog_entry && active_it->second.contains(parsed->key))`;
   - §6 SAFETY FLOOR: `:828-830` `if (catalog_entry) { ... manifestDeletionPremise(...) }`.
   Обоснование дано в комментарии `:822-826`: «If the post-observation catalog cut has no row, this candidate was necessarily created by a now-dead life: creation publishes its row before any life-owned object.»
   Далее — прямая номинация: `:875-895`, включая `for (const ManifestEntry & entry : body.entries) ... nomination.source_retirements.push_back(BlobSourceRetirement{...})`. Номинации исполняются в `Gc/CasGc.cpp:1241-1246` (`backend.deleteExact(nomination.key, nomination.token)`), retirements — `Gc/CasGc.cpp:3121-3140`.
   Для ЭТОГО пути утверждение findings подтверждается как факт кода.

Дополнительно: `catalog_recovery_authoritative` (в раунде — `universe_authoritative`, `CasGc.cpp:3124-3132`) тоже не применяется к ветке без строки, т.е. даже неавторитетный раунд удаляет такие тела.

## (b) Может ли попасть ПЕРВАЯ запись в namespace

Да, окно реально, и премисса комментария на HEAD неверна.

Порядок долговечных записей зафиксирован в `Pool/CasPartWriteTxn.h:109`: «The durable write order is `stageManifest` → `precommitAdd` → `putBlob` → `promote`».

- `PartWriteTxn::stageManifest` (`Pool/CasPartWriteTxn.cpp:809-911`) кладёт тело по ключу `store->layout().manifestKey(id)` (`:853`, PUT на `:878`) и НИГДЕ не разрешает жизнь namespace-а — ни `namespaceLife`, ни `readCkpt`.
- Строка каталога создаётся лениво, уже ВНУТРИ `precommitAdd`: `Pool/CasPartWriteTxn.cpp:953` `store->appendRefOps(target_ns, ...)` → `CasRefLedger::namespaceLife` (`Pool/CasRefLedger.cpp:4691-4740`) → `resolveNamespaceLife` (`:1213`), где на «нет строки вообще» вызывается `CasRefCatalog::createNamespace` (`Pool/CasRefLedger.cpp:1234-1240`), а тот вставляет `Creating`-строку только на `Pool/CasRefCatalog.cpp:579-590` (`const CatalogEntry entry{.ns = ns, .state = NsState::Creating, ...}; createNamespaceStep1(...)`).
- В `ContentAddressedTransaction.cpp:410-411` виден и сам порядок вызовов: `const Cas::ManifestId id = st.build->stageManifest(st.entries); st.build->precommitAdd(ns, ref, id);`.

Значит на интервале [PUT тела манифеста … вставка `Creating`-строки] в `cas/manifests/<ns>/` существует тело, для которого каталог не имеет строки. Чтобы sweep это удалил, окно должно накрыть LIST → freeze-GET → catalog-read одного прохода (`:636-644` freeze-GET, `:648-650` catalog cut), т.е. писатель должен провести между PUT и CAS каталога больше времени, чем GC тратит на эти операции. Это достижимо (каталог — один pool-глобальный contended объект с retry-циклом, `appendRefOps` идёт через очередь с лидерством), но требует ещё и совпадения курсора sweep-а с нужной страницей (`manifest_sweep_list_budget_keys` = 1000, `manifest_sweep_delete_budget_keys` = 100 — `ContentAddressedSettings.cpp:74-75`), т.е. вероятность мала. Того же вида окно есть у перерождения namespace-а после удаления строки. Формулировка findings «первая запись в namespace» — по механизму верна.

Ключевой вывод: не сам факт удаления debris неправ (для брошенного после `stageManifest` тела это и есть штатная уборка), а то, что durable-watermark-гейт `prefixEligible` — единственная защита, которая различила бы «активный build живого mount-lease» от «мусор мёртвой жизни» — на этой ветке не применяется. Строчка `:481-482` («отсутствующий floor = не eligible») и активный `min_active` живого писателя как раз задержали бы удаление.

## (c) Коммит 684161dcc03

Не относится к делу. `git log -1 --stat 684161dcc03` показывает изменения только в `Pool/CasRefLedger.cpp`, `Pool/CasRefLedger.h`, `src/Disks/tests/gtest_cas_ref_writer.cpp`; `Gc/CasOrphanManifestSweep.cpp` он не трогает (`git show 684161dcc03 -- .../Gc/CasOrphanManifestSweep.cpp` — пусто). Он про другой per-row-контракт: presence-probe и cold-reader admission больше не требуют побайтовой неизменности ВСЕГО каталога, а проверяют свою строку. Тема близкая («per-row вместо whole-catalog»), но конкретную ветку sweep-а он не закрывает и не создаёт. Сама ветка появилась раньше — по `git log -L 815,835:...CasOrphanManifestSweep.cpp` последние касания: `7ce8adcacaf` («§6 deletion premise»), `624811e6833`, `357cf7b963f`.

## (d) Громко или тихо

Скорее громко, и это резко снижает класс:
- писатель падает fail-closed на `promote`: `Pool/CasPartWriteTxn.cpp:1032-1039` — тело читается заново, `if (!body_got) throwCasWriteRetryLater("promote: manifest body absent at {} — failing closed (retry with a fresh ManifestId)")`. Т.е. подтверждённого INSERT-а с потерянным телом не получается; получается retryable-ошибка;
- `precommitAdd` тело НЕ проверяет (`Pool/CasPartWriteTxn.cpp:940-941`: «No body HEAD — a missing body is a legal fail-closed, non-activating intent»), так что precommit-биндинг успевает лечь и остаётся мусором до штатной уборки precommit-ов;
- сами удаления и retirements логируются событиями: `Gc/CasGc.cpp:1247-1259` (`CasEventType::ManifestDelete`, reason «orphan-manifest sweep: source edges retired and adopted before exact-token delete»);
- преждевременные `BlobSourceRetirement` для ещё не существующих рёбер безвредны по контракту редьюсера: `Gc/CasBlobInDegree.h:167-174` — «The reducer applies this as an idempotent exact-key removal ... an already-absent edge is not an unmatched-remove correctness signal». Отрицательного in-degree и досрочного осуждения чужих блобов из этого не выходит; блобы к этому моменту вообще ещё не загружены (`putBlob` идёт после `precommitAdd`).

Итого класс findings `DATA-LOSS` не подтверждается; реальный класс — «узкая гонка, теряющая INSERT-попытку и оставляющая precommit-мусор», плюс — важнее — ложная премисса в комментарии, на которую опирается пропуск ВСЕХ гейтов.

## BACKLOG / история

- `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` этого пункта не содержат. Ближайшие соседи — про другое: `BACKLOG/gc.md:53` (fsck недоучитывает orphan-манифесты у ref-less namespace-ов) и `BACKLOG/gc.md:138` (`orphan-sweep-byte-budget` — номинация ограничена числом объектов, а не байтами).
- `git log -S "creation publishes its row before any life-owned object" -- src` даёт единственный коммит `357cf7b963f` («ca: ref — LIST-independent recovery…»), т.е. премисса зафиксирована там и с тех пор не пересматривалась. Признаков «было, уже исправлено» нет.
- Тестов на эту ветку нет: в `src/Disks/tests/gtest_cas_orphan_nomination.cpp` тесты — `RetiresExactManifestSourcesBeforeDelete`, `CorruptManifestIsRetainedAndSurfaced`, `TokenAbaIsRetainedAndSurfaced`, `SuppressedRoundNominatesNothing`, `SourceRetirementIsAccountingNeutral`; кейса «строки каталога нет, а namespace жив и в середине первой записи» среди них нет.

## Что реально осталось

1. Применять `prefixEligible` (durable mount-lease floor) БЕЗУСЛОВНО, в том числе когда строки каталога нет: это ровно тот durable факт, которого у только что рождающегося namespace-а быть не может (у живого writer-а его build_sequence ≥ min_active ⇒ retain), а у мусора мёртвой жизни он есть.
2. Исправить комментарий `:822-826`: премисса «creation publishes its row before any life-owned object» неверна при текущем порядке `stageManifest` → `precommitAdd`(→создание строки). Либо менять порядок (резолвить жизнь namespace-а до первого `stageManifest`), либо снять премиссу.
3. Добавить регресс-тест на ветку без строки каталога (тело есть, строки нет, mount-lease floor удерживает build) в `gtest_cas_orphan_nomination.cpp`.

## CAS-023 — Оба поведения на HEAD подтверждены как факты кода, но класс `DATA-LOSS` не подтверждается: `gc_enabled=false` документирован как отладочный режим и уже стоит в BACKLOG, vanished-пул no-op-успех — сознательный fail-close контракт; неверна лишь формулировка «every manual reclamation verb is refused» (FSCK и `GC STOP` работают). (частично, P3) {#cas-023}

## Стале-анкеры

Анкеры findings (`CA/ContentAddressedMetadataStorage.cpp:611`, `:461-464`, `:492-494`, `:715-717`, `:809-812`, `CA/ContentAddressedTransaction.cpp:683`, `:705`, `:1069`) устарели. На HEAD файлы: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` и `.../ContentAddressedTransaction.cpp`; соответствующие места — `ContentAddressedMetadataStorage.cpp:603-604`, `:651-653`, `:858`, `:1016-1019`, `:1116-1167`; `ContentAddressedTransaction.cpp:446-455`, `:1006-1007`, `:1044-1045`, `:1581-1582`.

## (a) `gc_enabled=false`: принимается ли удаление и какие вербы отказывают

Удаление ПРИНИМАЕТСЯ — подтверждено:
- `gc_enabled` в CA вообще существует только в четырёх местах metadata-storage (`ContentAddressedMetadataStorage.cpp:603`, `:651`, `:858`, `:1016`) и НИ РАЗУ в `ContentAddressedTransaction.cpp` (grep по файлу — пусто). Пути удаления его не читают: `ContentAddressedTransaction.cpp:996-1002` — «Removal = pointer-unlink + deferred GC: only refs and verbatim files go; the shared blobs/trees are reclaimed by `Cas::Gc` once unreachable», т.е. DROP/`removeRecursive`/`unlinkFile` снимают ссылки и завершаются успехом независимо от `gc_enabled`.
- Фоновый планировщик не поднимается: `ContentAddressedMetadataStorage.cpp:858` `if (context && gc_enabled && !read_only)` — при `false` `CasGcScheduler` не создаётся вообще.

Ручные вербы — заявление findings ВЕРНО ЛИШЬ ЧАСТИЧНО:
- `SYSTEM CAS GC RUN` → `runGarbageCollectionRoundNow`, `ContentAddressedMetadataStorage.cpp:602-605`: `if (!gc_enabled) throw Exception(ErrorCodes::BAD_ARGUMENTS, "Garbage collection is not enabled on this content-addressed disk");` — подтверждено.
- `SYSTEM CAS GC REBUILD` → `runGcRebuildNow`, `:650-653` — тот же `BAD_ARGUMENTS`, подтверждено.
- `SYSTEM CAS GC START` → `gcStart`, `:1015-1019` — тот же `BAD_ARGUMENTS`, подтверждено.
- `SYSTEM CAS GC STOP` → `gcStop`, `:980` и `:995-1005`: гейта на `gc_enabled` НЕТ, при отсутствии планировщика печатается INFO «no GC scheduler (disabled/read-only/not started) -- nothing to stop» и возвращается успех.
- `SYSTEM CAS FSCK` → `runFsckNow`, `:1049-1070`: `gc_enabled` НЕ проверяется вообще, только `checkOpAdmitted(CasOpClass::Admin)`. То есть диагностика (в т.ч. поиск мусора) при `gc_enabled=false` полностью доступна.

Итог по (a): при `gc_enabled=false` действительно нет способа выполнить реклейм без правки конфига и перемонтирования диска (три верба из пяти отказывают BAD_ARGUMENTS), но формулировка «every manual reclamation verb is refused» неточна — FSCK работает, STOP работает.

## (b) Vanished-пул: что делает удаление и что видит вызывающий

Подтверждено буквально, но это ЗАДОКУМЕНТИРОВАННЫЙ контракт, а не побочный эффект:
- `checkOpAdmitted`, `ContentAddressedMetadataStorage.cpp:1160-1164`: `const bool settled_vanished = lc == Cas::PoolLifecycle::VanishedReplaced || lc == Cas::PoolLifecycle::VanishedForgotten; if (settled_vanished && (op == CasOpClass::Probe || op == CasOpClass::Remove)) return CasOpAdmission::TruthAbsent;`
- Классовый контракт зафиксирован в шапке: `ContentAddressedMetadataStorage.h:66-68` — «`Remove` — ref/file removal ... No-op SUCCESS on `Vanished` so a vanished-disk table's `DROP` completes» и `:251-253` — «`Vanished*` -> `Probe`/`Remove` answer `TruthAbsent` (absent-empty / no-op success)».
- Сайты: `ContentAddressedTransaction.cpp:446-455` (пустой commit = `Remove` → `committed = true; return;`), `:1006-1007`, `:1044-1045`, `:1581-1582` (везде `if (... == CasOpAdmission::TruthAbsent) return;`).

Почему это НЕ data-loss и почему это fail-close:
- `VanishedReplaced` — префикс пула перезаписан ЧУЖОЙ идентичностью; удалять там объекты было бы кросс-пуловым разрушительным действием. `VanishedForgotten` — оператор сам выполнил `FORGET`. В обоих случаях «нечего удалять» — правдивый ответ для этого диска, а не потеря данных.
- Промежуточные состояния наружу не «тихие»: `TransientNotLive` и `IdentityLost` бросают типизированный 668 для всех классов (`:1139-1157`), т.е. DROP пере-очередится, а не соврёт.
- Состояние видно в `system.cas_mounts` (`ContentAddressedMetadataStorage.h:283-300`: `lifecycle`/`reason`, «vanished(forgotten)» / «vanished(replaced)»), причём это Factory-класс — правдив в любом состоянии.

Что действительно остаётся: сам no-op возвращается БЕЗ единой строчки лога на уровне операции (`return;` в четырёх местах выше) — оператор узнаёт состояние только из `system.cas_mounts`/логов монтирования. Это дефект наблюдаемости, а не корректности.

## (c) Реально ли противоречие «принимаем удаления, но отказываем в единственном способе реклейма»

Противоречие существует в узкой форме и уже названо в документации как осознанный компромисс:
- `docs/en/antalya/cas/configuration.md:88`: «`gc_enabled` | `true` | Run the background GC scheduler on this disk. `false` is a debugging aid, not an operating mode: garbage then accumulates indefinitely and silently — watch `system.cas_gc_log` for round activity if you ever toggle it». Т.е. «принимаем удаления, мусор копится» — заявленное поведение отладочного тумблера, а не режима эксплуатации.
- `docs/en/operations/storing-data.md:525`: «`gc_enabled` — `true` by default. Enables the background garbage collector for this disk.»
- Важный нюанс, который findings не учитывает: `gc_enabled` — настройка МОНТИРОВАНИЯ, не пула. Лизу GC делят все монтирующие (`ContentAddressedMetadataStorage.cpp:846-848`: «the lease makes concurrent schedulers across mounters safe (work dedup)»), поэтому `gc_enabled=false` на одном узле не означает, что пул никто не чистит. Полное отсутствие реклейма — только если он выключен у ВСЕХ монтирований.

Остаточная часть противоречия: отказ `GC RUN` при `gc_enabled=false` заставляет менять конфиг, тогда как раунд технически исполним (верб сам создаёт ленивый планировщик — `:631-644`). Это вопрос эргономики верба, P3.

Дополнительно: тезис pre-filled verdict-а «operator can `GC RUN` anytime» сам по себе имеет оговорку и при `gc_enabled=true` — см. `docs/superpowers/cas/BACKLOG.md:744` `{#issue-2211-gc-run-follower-noop}`: на не-лидере `SYSTEM CAS GC RUN` тихо ничего не делает (адъюдицировано 2026-08-21, решение — оставить idempotent OK, но сделать исход видимым колонкой `finish` и advisory-идентичностью в `GcLease`).

## Работа по редизайну жизненного цикла диска — изменила ли гейты

Частично, и в основном не эти гейты:
- FSCK на HEAD НЕ привязан к «dormant»-состоянию и не гейтится `gc_enabled` (`:1049-1070`) — цель «FSCK not dormant-only» в этой части выполнена.
- `GC STOP`/`GC START` существуют как вербы (`gcStop` `:980`, `gcStart` `:1012`), появились в `e79a109b142` («ca: GC STOP/START verbs — restartable scheduler, leadership cleared on stop»); при этом BACKLOG всё ещё держит `BACKLOG/operability-and-introspection.md:17` `[B197]` («SYSTEM control surface — START/STOP GC, POOL READONLY, CHECK — GATE») как незакрытый зонтичный пункт.
- Машинерия Dormant/UNMOUNT/MOUNT откачена: `BACKLOG/mounts-and-lifecycle.md:34` — «The Dormant/UNMOUNT/MOUNT reuse machinery that pursued this was rolled back (spec rev.8 §9)»; `BACKLOG.md:479` — «(`UNMOUNT` stops background work and ejects the disk) subsumes this half», т.е. UNMOUNT остаётся целью, а не реализацией. Так что на гейты `gc_enabled` и на vanished-`TruthAbsent` редизайн пока не повлиял.

## BACKLOG и история

- Половина про `gc_enabled=false` УЖЕ ОТСЛЕЖИВАЕТСЯ: `docs/superpowers/cas/BACKLOG.md:416`, пункт `[gc-enabled-false-silent]` в разделе `{#consolidation-2026-08-findings}` (заголовок — `BACKLOG.md:404`): «Disabling the background GC scheduler produces no ongoing signal that reclamation has stopped. Add a periodic warning log line plus a metric while `gc_enabled=false` and the pool has reclaimable debris…». Класс — HARD (направление политики настроек), не блокер релиза.
- Половины про vanished-пул в BACKLOG нет — потому что это описанный контракт (`ContentAddressedMetadataStorage.h:66-68`, `:251-253`), а не открытый дефект.
- `git log -S "Garbage collection is not enabled on this content-addressed disk" -- src` → `2ab002c1b4e`, `44d06a1e57a`, `e79a109b142`: гейт добавлялся вместе с самими вербами (GC REBUILD, GC STOP/START); признаков «было иначе, потом сломали» нет.

## Что реально осталось

1. `[gc-enabled-false-silent]` (уже в BACKLOG): периодический WARNING + метрика, пока `gc_enabled=false` и в пуле есть освобождаемый мусор.
2. Эргономика: разрешить одноразовый `SYSTEM CAS GC RUN` (и, возможно, `REBUILD`) при `gc_enabled=false` — верб и так создаёт планировщик лениво, а отказ BAD_ARGUMENTS вынуждает менять конфиг ради ручного реклейма. Либо — оставить отказ, но в тексте ошибки назвать путь («set `gc_enabled=1` or run GC from another mount of this pool»), т.к. сейчас сообщение не подсказывает выхода.
3. Наблюдаемость no-op удаления на vanished-пуле: одна LOG_INFO-строка (или счётчик ProfileEvents) на `TruthAbsent`-возврат в `ContentAddressedTransaction.cpp:446/1006/1044/1581`, чтобы «DROP прошёл, а физически ничего не тронуто» читалось из логов, а не только из `system.cas_mounts`.
4. Класс findings `DATA-LOSS` следует понизить: ни один из двух путей не теряет подтверждённых данных.

## CAS-026 — Идентичность relink — не «только `pool_uuid`»: это pool_uuid + server_root_id + namespace + ref + part + точный `ManifestRef`, доказываемый publish-then-confirm у источника; отсутствие probe блобов — сознательный §4 manifest-trust (`8fe6331a431`), а `check_consistency=false` — байт-в-байт то же, что и в upstream байтовом пути. (by-design, P3) {#cas-026}

**(a) Какую идентичность реально требует получатель/отправитель.**

Утверждение «оба гейта — одно и то же равенство `pool_uuid`» на HEAD неверно; равенство `pool_uuid` — это только гейт *предложения*, а не гейт *публикации*.

1. Гейт отправителя: `src/Storages/MergeTree/DataPartsExchange.cpp:410` — `if (ca_meta && !receiver_pool_uuid.empty() && receiver_pool_uuid == ca_meta->getPoolUUID())`, плюс версионный гейт протокола `DataPartsExchange.cpp:405` (`REPLICATION_PROTOCOL_VERSION_WITH_CA_CONFIRM`, а не `..._WITH_CA_RELINK`: получатель, не умеющий confirm, получает байты).
2. Повторная проверка у получателя ПОСЛЕ резервирования диска: `DataPartsExchange.cpp:927` — `if (!chosen_ca || chosen_ca->getPoolUUID() != advertised_pool_uuid)` → byte-fallback. Это уже было закрыто отдельно коммитом `f3cd6e1ff1f` («cas: fetch — re-check pool uuid after reservation, byte-fallback on mismatch (triage #7)»).
3. Токен, который получатель обязан вернуть источнику: `ContentAddressedExchange.h:44-53` — `CasRelinkSourceToken{pool_uuid, server_root_id, root_namespace, ref_name, part_name, manifest_ref_text}`; минтится источником из его СОБСТВЕННОГО закоммиченного состояния в `ContentAddressedMetadataStorage.cpp:2110-2116` (`manifest_ref_text` = именно тот манифест, который отдал `getView(..., Freshness::ForceFresh)`).
4. Публикация без подтверждения невозможна: `DataPartsExchange.cpp:1417` — отсутствие токена ⇒ `return nullptr` (байты); `:1528` — `source_proved_the_binding = ... == CA_CONFIRM_ANSWER_PROVEN`; `:1549` — при недоказанном confirm бросается `NETWORK_ERROR` (retry-later), а НЕ байтовый фолбэк к тому же источнику. Маршрутизация ответа: `DataPartsExchange.cpp:210-241` требует ровно одного диска с совпавшим `pool_uuid` И `ownsNamespace(server_root_id, root_namespace)` (иначе `Unknown`), где `ownsNamespace` (`ContentAddressedMetadataStorage.cpp:2009-2023`) — строгое равенство srid + строгий префикс `srid + "/"`, а собственно доказательство даёт `confirmExactRef` по точному `ManifestRef` (`:2026-2074`). Гейт 0 по parts-set явно помечен как availability-фильтр, «every `Yes` is earned by gate 1 alone» (`DataPartsExchange.cpp:243-250`).

Итог: «отправитель, рекламирующий совпадающий `pool_uuid`, но указывающий на другой физический префикс» не может добиться публикации — публикация авторизуется только ответом ЕГО ЖЕ mount'а о том, что он до сих пор держит ровно этот манифест в этом namespace.

**(b) Есть ли presence/validation зависимостей манифеста и сознательно ли её нет.**

Фактическая часть находки верна: presence-проверки блобов нет ни до precommit, ни на promote.
- `Parts/PartFolderAccess.cpp:478-480`: «Record write evidence for each non-inline entry. No pool HEAD/GET is performed before precommit; the promote path re-proves each dependency fail-closed.»
- `Pool/CasPartWriteTxn.h:218-223` (шаг 3 контракта promote): «a committed-source `adoptEvidence` leaf is TRUSTED via the durable manifest edge — NO per-file HEAD/loadMeta probe (§4 manifest-trust: the live source pins the blob, in-degree >= 1); a genuinely absent adopted blob is an invariant violation caught by fsck, not here».
- `ContentAddressedMetadataStorage.cpp:2241-2243` — то же самое явно повторено на call-site relink.

И это задокументированное решение, а не упущение: `ContentAddressedMetadataStorage.cpp:2209-2214` — «TRUST MODEL: adopting a part from a peer-supplied manifest is exactly as trusted as an ordinary ReplicatedMergeTree interserver part fetch… relink-by-manifest adds no new trust surface. (See the retracted umbrella "RBAC bypass" finding.)» История: `8fe6331a431` («cas: opt §4 — manifest-trust relink adoption (no per-file probes, no tokens)», 2026-07-14) сознательно заменил прежний per-leaf HEAD+`.meta` доверием к durable manifest edge с принятым trade-off (отсутствующий блоб уходит из promote в fsck reachable-but-absent scan, D4); `76167f5cbd7` («cas: document relink trust model == ReplicatedMergeTree interserver fetch (no per-blob ACL)») зафиксировал позицию в коде. Урегулированная позиция «fetch-by-relink == обычный interserver-trust ReplicatedMergeTree» коду на HEAD соответствует.

То, что находка называет «unverified publish», на HEAD прикрыто ортогональным механизмом: publish-then-confirm (`260a6f81169`, `8e6fe6ef0af`) закрывает окно commit-before-release источника — precommit получателя durable ДО вопроса источнику, поэтому любое удаление исходной привязки аппендится строго после `+1`. В коде честно указан и предел этой гарантии: `DataPartsExchange.cpp:1380-1386` («What a `yes` does NOT prove»: с одной неполной страницей LIST `ConfirmedRelinkNeverDangles` ломается, BACKLOG `{#list-as-journal-dataloss-2026-07-25}`) — то есть остаточный риск dangling существует, но он уже отслеживается как LIST-trust класс, а не как дефект relink.

**(c) Что ломается при совпадении `pool_uuid` у двух разных бакетов.**

`pool_id` — 128-битное случайное значение, минтится один раз при создании пула: `Pool/CasPoolMeta.cpp:25-30` (`mintPoolId`, две выборки `thread_local_rng`), `:149` `pm.pool_id = mintPoolId()`; формат — `Formats/CasPoolMetaFormat.h:23-26` («`pool_id` is also the envelope `domain_id`, so it remains stable for the entire pool lifetime»). Случайная коллизия исключена (2^-128). Единственный реалистичный путь к «двум бакетам с одним `pool_uuid`» — побайтовое КОПИРОВАНИЕ пула (DR-клон/зеркало бакета или префикса) с последующим включением обеих копий в один replicated-набор; это операторская ошибка конфигурации, а не путь атаки. Последствие в этом сценарии — не «чужие данные», а именно dangling: получатель адоптирует по хешу блоб, которого в ЕГО копии нет (после расхождения копий), и это ловит fsck (`reachable-but-absent`) — то же место, куда §4 сознательно вынес отсутствующий адоптированный блоб. Отмечу смежный (устаревший по формулировке) пункт `docs/superpowers/cas/BACKLOG/formats-and-storage.md:80` `[mixed-ca-tiered-topology]` — «Governs the severity of a relink pool-UUID mis-advertise bug»; отдельного item я не завожу: mis-advertise на HEAD не даёт публикации (см. (a)), а клон-бакет — конфигурационный анти-паттерн.

**(d) `check_consistency=false`.**

Здесь находка просто ошибается фактически. Указанного `DataPartsExchange.cpp:1262` не существует; речь о `new_data_part->loadColumnsChecksumsIndexes(true, false)`. Байтовый путь: `DataPartsExchange.cpp:1245`, `git blame` → `499f678112ef` (alesapin, 2022-09-21) — upstream, до CAS. Relink-путь: `DataPartsExchange.cpp:1591`, `fa27021e03bb` — ровно та же пара аргументов. То есть «получатель загружает парт без валидации, которую делает байтовый путь» неверно: `check_consistency` в обоих путях `false`, это не выбор CAS и не perf-решение relink'а, а паритет с upstream. `git log -S check_consistency -- src/Storages/MergeTree/DataPartsExchange.cpp` — пусто (значение никогда не менялось).

Единственное настоящее отличие: байтовый путь дополнительно делает `new_data_part->checksums.checkEqual(data_checksums, false, name)` (`DataPartsExchange.cpp:1280`) — сверку контрольных сумм ПОТОКА с `checksums.txt`. У relink'а аналога нет по построению: байты не передаются, передаётся только манифест, чья целостность обеспечивается декодером (`decodePartManifest` ⇒ `CORRUPTED_DATA` на пути `ContentAddressedMetadataStorage.cpp:2262-2271`) и content-addressed идентичностью блобов. Транспортной сверки для нулевого транспорта не требуется.

**BACKLOG/история.**

Прямого anchor'а по CAS-026 нет. Класс уже адъюдицирован в этом же прогоне как CAS-002 (`docs/superpowers/cas/2031-triage.md:20,258-281`, by-design, P3): «Отсутствие probe/condemn-check в `adoptEvidence` — сознательный дизайн §4 manifest-trust… остаток — принятый D4 trade-off с fsck-backstop». CAS-026 — та же сердцевина плюс две ложные добавки (`pool_uuid` как единственная идентичность; `check_consistency` как отличие relink'а), поэтому по существу это почти-дубликат CAS-002; отдельным дефектом не является. Смежные отслеживаемые остатки, которые НЕ являются этой находкой: `BACKLOG.md {#list-as-journal-dataloss-2026-07-25}` (confirmed relink не доказан dangle-free), `BACKLOG/replication.md:20` `[RPL-5 slice]` (нет теста, что клонированный `REPLACE PARTITION` реально relink'ает), `BACKLOG/testing-and-ci.md:62` `[relink-positive-proof-log-line]`.

**Что реально осталось.** Ничего кода-уровня. P3 — только гигиена прозы/тестов: устаревшая формулировка `[mixed-ca-tiered-topology]` про «relink pool-UUID mis-advertise bug» (на HEAD mis-advertise не публикует ничего) и отсутствие positive-proof теста, что relink состоялся, — оба уже в BACKLOG.

## CAS-027 — Код на HEAD ровно соответствует урегулированной позиции (никакой intra-pool аутентификации нет, bucket-credential = вся граница доверия); единственный реальный остаток — эта граница доверия не описана нигде в `docs/en/antalya/cas/`. (by-design, P3) {#cas-027}

Урегулированную позицию не пересматриваю. Проверял только два пункта: (a) держится ли она на коде и (b) сказано ли это оператору.

**(a) Код на HEAD подтверждает механику полностью — intra-pool аутентификации нет.**

Все контрольные объекты пула защищены только условной записью (токен/`putIfAbsent`), то есть ровно от РАСХОЖДЕНИЯ, и ни в одном месте — от того, КТО пишет:

- Retire (постоянное отключение участника): `Pool/CasServerRoot.cpp:75-85` `throwIfOwnerRetired` — при непустом `owner.retired_at_ms` старт server-root'а падает `CORRUPTED_DATA` с требованием «manually clear the owner object's tombstone field and restart». То есть одна запись в `gc/server-roots/<victim>/owner` с выставленным `retired_at_ms` отключает жертву до РУЧНОГО вмешательства оператора. Томбстоун вместо удаления — сознательное решение (`9707a61ba2c` «cas: decommission tombstones the owner anchor instead of deleting it (triage #9)»), и санкционированный путь к нему — `SYSTEM CAS DROP POOL MEMBER` (`docs/en/antalya/cas/operations/migration.md:163-178`), но сам объект ничем не защищён от произвольного писателя.
- Claim/identity: `CasServerRoot.cpp:112-175` `claimOwnerOrThrow` — идентичность решается по `owner->server_uuid` из САМОГО объекта; при чужом UUID — fail-closed. Это защищает от случайного двойного старта, а не от подделки: `server_uuid` — это самозаявленное содержимое объекта, которое любой держатель креденшелов может записать.
- Fence (отъём права на запись): `CasServerRoot.cpp:659-665` — `MountLease fenced = m; fenced.gc_fenced = true; fenced.seq = m.seq + 1; b.putOverwrite(key, encodeMountLease(fenced), got->token)`. Это ОБЫЧНЫЙ продуктовый путь (`computeHeartbeatFloor`, `:570`): любой участник, ставший GC-лидером, пишет `gc_fenced` в слот чужого mount'а; гейт — только token-stability наблюдение (`:640-657`), никакой авторизации. Для жертвы это терминально по построению: `CasServerRoot.cpp:388-397` — своя же `(uuid, epoch)` пара с `gc_fenced` даёт `FencedSelf`, «terminal for this incarnation».
- Кража слота: через продуктовый протокол чужой слот НЕ отбирается — `claimMount` при `existing.server_uuid != our_uuid` возвращает `ForeignOwner` и не пишет (`CasServerRoot.cpp:377-383`), а same-uuid reclaim требует сертификата смерти (`gc_fenced` / clean marker / `proven_dead_token`, `:427-445`). Поэтому формулировка находки «one PUT ... steals its slot» верна только для СЫРОЙ записи в бакет в обход протокола (записать в `gc/server-roots/<victim>/mount` тело со своим `server_uuid`, либо предварительно поставить `gc_fenced` и затем забрать слот) — что и есть ровно та модель, которую позиция объявляет вне защиты. Двухшаговость («сначала fence, потом claim») ничего не меняет: fence-шаг доступен как обычная запись.

Итог по (a): ни одного места, где бы проверялось право писателя на объект другого участника, на HEAD нет. Код соответствует позиции «bucket credential = whole trust boundary; all pool users same trust». Ничего исправлять не нужно; и по конструкции пула (нет внешнего координатора: `docs/en/antalya/cas/index.md:41-45` — «Every CAS bookkeeping object … lives in the bucket. There is no external coordinator») внутрипуловая аутентификация и не может появиться без ввода новой сущности.

**(b) Документация — единственный actionable остаток, и он реален.**

`grep -rni "trust|credential|security|permission|IAM|access key" docs/en/antalya/cas/` не находит ни одного заявления о границе доверия пула. Все попадания — про другое: `bucket-requirements.md:14` (условия бакета «refused rather than trusted»), `configuration.md:101` (`part_folder_validate` — «trust decision about unverified data»), `architecture/blob-protocol.md:95-98` (выбор `sha256` для «untrusted writers» — про содержимое блобов, не про участников), `architecture/read-path.md:62`, `architecture/replication.md:69` (получатель relink'а «trusts nothing from the wire but the entry list»). В `index.md` есть только «Deployment guidance» про GC/шардирование (`:47-66`) и «Status» (`:67-77`); раздела про безопасность/креденшелы нет. `bucket-requirements.md` — только capability-таблица и platform support (`:16`, `:33`).

Чего конкретно не сказано оператору: что держатель креденшелов пула может retire'нуть участника (томбстоун `owner`), зафенсить его записи (`gc_fenced`) или занять его mount-слот; что префикс пула нельзя отдавать роли/тенанту, которому не доверена доступность ВСЕХ участников; что backup/log-shipping/аналитические роли, направленные на пул, должны быть read-only. Оператор не может вывести требование, которое нигде не написано (та же логика, по которой уже принят CAS-012).

**BACKLOG/история.**

Прямого anchor'а по этой теме не было. Ближайший смежный — `BACKLOG/formats-and-storage.md:84` `[sec4-decoder-size-bounds]` («A pool-write-capable party could place an enormous but validly-framed object»), то есть модель «pool-write-capable party» в BACKLOG уже используется как данность, но нигде не описана в пользовательских доках. `git log -S "retired_at_ms"` показывает только слои-переносы CAS-подсистемы и `9707a61ba2c` (томбстоун вместо удаления) — никакого «since fixed» коммита, вводящего intra-pool auth, в истории нет; `git log -i --grep="trust boundary"` по CAS-ветке даёт только upstream-скиллы ревью, не код пула.

Под этот остаток заведён новый пункт: `docs/superpowers/cas/BACKLOG/docs-and-cleanup.md` `{#pool-trust-boundary-undocumented}`.

**Что реально осталось.** Только докстроки (P3, не pre-release-блокер): короткая секция про границу доверия в `index.md` или `bucket-requirements.md`. Кодовых изменений находка не требует.

## CAS-030 — Механика верна (probe целиком пропускается, decommission выставляет флаг жёстко), но «removes every bucket-configuration defense» преувеличено — single-attempt gate и residual-proof остаются, а реальный остаток — отсутствие ЛЮБОГО сигнала оператору о пропущенном probe и самопротиворечивое сообщение про versioning «has no override». (частично, P3) {#cas-030}

**Где сейчас лежит код (анкеры находки устарели).** `CA/Pool/CasPool.cpp:339-347` → `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:457-481`; `:528` (жёсткий skip) → тот же файл `:809`; `CA/ContentAddressedSettings.cpp:35` → `ContentAddressedSettings.cpp:69`; `CA/Backend/CasProbe.cpp:15-66` → `Backend/CasProbe.cpp:15-243` (батарея выросла до 9 шагов). Файлы переехали в подкаталоги коммитом `592b9b83568` (git mv), номера строк в находке не соответствуют HEAD ни в одном из четырёх анкеров.

**(1) «`skip_access_check` пропускает probe» — ПОДТВЕРЖДЕНО, но split явный и запинен тестами.**

`Pool/CasPool.cpp:457-481`:
```
if (!config.skip_access_check)
{
    const UInt128 probe_uid = ...;
    runCapabilityProbe(*backend, config.pool_prefix + "/_probe/" + u128ToHex(probe_uid));
}
else
{
    backend->checkConditionalWriteSingleAttemptSupport();
}
```
Т.е. пропускается вся батарея `Backend/CasProbe.cpp:47-243` (шаг 0 `checkPoolPreconditions`, шаги 1-8: enforcement conditional-create/overwrite/`casPut`/`deleteExact`, list-after-write/delete, детект delete-marker'а), а `checkConditionalWriteSingleAttemptSupport` (`Backend/CasProbe.cpp:53`, дублирован в `else`-ветке) выполняется всегда на writable-монтировании. Это НЕ «wrap the whole probe»: split заведён специально (`fb25e8cd3f6`, 2026-07-13, «cas: skip the capability probe under skip_access_check») и запинен дискриминирующим тестом `CASPool.SkipAccessCheckStillEnforcesSingleAttemptGate` (`src/Disks/tests/gtest_cas_pool.cpp:255-269`, комментарий `:226-229` прямо называет регрессию, от которой он защищает) плюс `692cea3ab04` (2026-07-13, «pin the skip_access_check safety split with discriminating tests»).

**(2) «removes EVERY bucket-configuration defense» — ПРЕУВЕЛИЧЕНО.**

Что остаётся при `skip_access_check=true`:
- single-attempt gate (`Backend/CasObjectStorageBackend.cpp:94-108`) — writable-монтирование на объектном хранилище, не поддерживающем профиль `SingleAttempt`, по-прежнему отказывается монтироваться (`NOT_IMPLEMENTED`);
- zero-write residual-proof перед bootstrap'ом (`Pool/CasPool.cpp:392-455`) — `ResidualWithoutMeta`/`Indeterminate` по-прежнему fail-closed;
- `PoolMeta::createOrValidate` (`Pool/CasPool.cpp:499-502`) — сверка `blob_header_len`/`gc_shards`/`algos_used` и запрет mint'а на read-only открытии.

Что действительно теряется — только доказательство enforcement'а условных операций ЭТИМ путём и два versioning-детектора: GCS-специфичный `checkPoolPreconditions` (`Backend/CasObjectStorageBackend.cpp:56-84`; для ETag-хранилищ это no-op: `:58-59` `if (mode != Mode::Native || native_token_type != TokenType::Generation) return;`) и шаг 8 `created_delete_marker` (`Backend/CasProbe.cpp:196-206`) — последний работает и на AWS, и он единственный «bucket-configuration defense» общего вида. Комментарий в `Pool/CasPool.cpp:475-478` эту потерю признаёт и квалифицирует как «purely environmental (slower GC reclaim, not data loss)» — с этим согласен: versioned bucket даёт неубираемый рост, а не потерю данных.

**(3) Реальный остаток №1: сообщение шага 8 противоречит существованию опции.** `Backend/CasProbe.cpp:203-205` утверждает: «This is NOT ignorable and **has no override**» — при том что `skip_access_check=true` ровно этот шаг и снимает. Это не «дыра», это ложное утверждение в user-facing тексте (P3, правится одной фразой: «…кроме случая, когда probe пропущен `skip_access_check`, тогда проверка отложена до следующего обычного монтирования»).

**(4) Реальный остаток №2 (главный): opt-out нигде не наблюдаем — ПОДТВЕРЖДЕНО.**
- в `else`-ветке (`Pool/CasPool.cpp:470-480`) нет ни одной строки лога: пропуск батареи проходит абсолютно молча;
- в пуле не записывается: `_pool_meta` несёт только `pid`/`hln`/`gcs`/`mrg`/`alg` (`Formats/CasPoolMetaFormat.cpp:76-87`) — поля про probe нет;
- в интроспекции не видно: `system.cas_mounts` (`src/Storages/System/StorageSystemContentAddressedMounts.cpp:40-59`) — 20 колонок, ни одной про capability-probe/skip.

Итог: узел, поднятый с `skip_access_check=true`, внешне неотличим от проверенного, включая для второго writer'а того же shared-пула. Это тот же класс, что уже зафиксирован в BACKLOG под `{#consolidation-2026-08-findings}`: `[gc-enabled-false-silent]` («Disabling the background GC scheduler produces no ongoing signal…») и `[part-folder-validate-never-gating]` («accepts `never` … with no acknowledgment of the risk», помечено HARD (user settings-policy direction)). Минимальный фикс того же вида: `LOG_WARNING` при пропуске + строка в `system.cas_mounts`/`_pool_meta`-независимая метрика. Отдельного анкера именно про `skip_access_check` в `BACKLOG.md`/`BACKLOG/*.md` нет (единственные совпадения по «probe» — про LIST-passport `{#list-consistency-real-s3}` и GC-probe'ы, не про это).

**(5) «the shipped description invites the setting» — формально да, по существу нет.** Строка `ContentAddressedSettings.cpp:69` — «Skip the boot-time capability probe (start now, fix later)»; «start now, fix later» — это цитата семантики upstream'ного `skip_access_check` (`src/Disks/IDisk.cpp:217-223`, `docs/en/operations/storing-data.md:552`), а не приглашение. Пользовательская документация при этом аккуратна и НЕ преувеличивает безопасность в обратную сторону: `docs/en/antalya/cas/configuration.md:92` — «Safer than the name suggests: only the preflight probe is skipped — the conditional-write correctness check still runs unconditionally on every writable mount». Т.е. документированная позиция совпадает с кодом (п. 1-2). Претензия «shipped description invites it» — стилистическая, P3 максимум.

**(6) «every decommission remount takes it unconditionally» — ПОДТВЕРЖДЕНО как код, следствие мягче заявленного.**

`Pool/CasPool.cpp:809`: `config.skip_access_check = true;   /// the pool exists (the calling disk validated it); no probe writes` (введено `03b3b95de44`, 2026-07-15). При этом `:834` — `backend->checkConditionalWriteSingleAttemptSupport();` вызывается явно, т.е. и здесь пропущена только батарея. Проверка обоснования по call site'ам:
- `SYSTEM CAS DROP POOL MEMBER` (`src/Interpreters/InterpreterSystemQuery.cpp:1063-1077`) переиспользует `host_store->poolConfig()` живого CA-диска — этот диск монтировался writable и батарею уже прогнал (если только сам не поднят с `skip_access_check=true`). Здесь пропуск — законная дедупликация проверки, ровно как написано в комментарии.
- `clickhouse-disks cas-drop-member` (`programs/disks/CommandCaDropMember.cpp:47-56`) — наоборот, ТРЕБУЕТ read-only открытия диска (`if (!ca->isReadOnly()) throw`), а read-only открытие probe не запускает вообще по построению (`Pool/CasPool.cpp:381` — вся секция под `if (!config.read_only)`, обоснование `:376-379`). Значит в этом пути enforcement условных операций не доказан НИ РАЗУ в процессе, а `openForDecommission` затем делает destructive-записи (удаление namespace'ов, staging, mount-слота, `Tools/CasDecommission.cpp:110-133+`). Формулировка комментария `:809` («the calling disk validated it») для этого пути верна буквально (диск подтвердил существование пула), но не покрывает capability.

Насколько это опасно: удаления в decommission идут через `deleteExact` (exact-token), т.е. на хранилище, игнорирующем `If-Match`, они могли бы снести чужую инкарнацию — но точно тот же аргумент относится к любому GC-раунду на таком хранилище, и такое хранилище не прошло бы обычный writable-mount на любом узле. Т.е. это не отдельная дыра decommission'а, а тот же «неcертифицированный store» из п. 4, просто без единственного места, где сертификация вообще могла бы случиться в этом процессе. Разумный минимальный фикс — прогонять батарею в `openForDecommission`, когда вызывающий диск был открыт read-only (или требовать флаг), плюс лог из п. 4.

**Чего в находке нет и что стоит зафиксировать отдельно:** ничего из перечисленного не является silent corruption — все сохранившиеся отказы громкие (`NOT_IMPLEMENTED`/`INVALID_STATE`), а всё пропущенное деградирует либо в отложенную проверку (следующее монтирование без флага), либо в стоимость (versioned bucket). Поэтому P3, не P1, и `PRE-RELEASE: нет`.

## CAS-031 — Верно: обе write-once CREATE-примитивы (streaming `putIfAbsentStream` и server-side `promoteStaged`) для больших тел переносят `If-None-Match` на `CompleteMultipartUpload`, а обе probe-проверки экзерсайзят только маленький single-operation путь — т.е. батарея сертифицирует не тот путь, по которому идёт основной объём блобов; но эксплуатируемо только на сторонних S3-совместимых хранилищах (AWS требование поддерживает, GCS отказывается громко). (подтверждено, P2) {#cas-031}

**Где сейчас лежит код (анкеры устарели).** `CA/Backend/CasObjectStorageBackend.cpp:632-636` → `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:914-931` (`putIfAbsentStream`); `CA/Backend/CasProbe.cpp:42` → `Backend/CasProbe.cpp:59-66` (шаг 1, `putIfAbsent` fresh). Анкер `src/IO/WriteBufferFromS3.cpp:409-416` указывает на `createMultipartUpload`'s GCS-refusal (сейчас `:407-416`) — это как раз ЗАКРЫТАЯ половина проблемы, а не открытая.

**(1) Probe проверяет только single-PUT — ПОДТВЕРЖДЕНО.**

Батарея пишет строковые литералы: `Backend/CasProbe.cpp:59` `backend.putIfAbsent(key, "probe-v1")`, `:75` `"should-not-land"`, `:104` `"clobbered"`, `:118` `"probe-v2"`, `:131-166` `"cas-s1"`/`"cas-s2"` — 8-9 байт. `WriteBufferFromS3::preFinalize` (`src/IO/WriteBufferFromS3.cpp:176-193`) выбирает single-part, пока `multipart_upload_id.empty() && detached_part_data.size() <= 1 && data_size <= max_single_part_upload_size` — т.е. любой шаг батареи гарантированно уходит одним `PutObject`. Ни один шаг не пишет тело, превышающее буфер/порог, и в батарее нет отдельного multipart-шага. То же для опциональной проверки server-side copy: `probeConditionalCopy` (`Backend/CasProbe.cpp:250-263`) пишет строку `"cas-conditional-copy-probe"` (26 байт) и копирует её — заведомо single-operation `CopyObject`.

При этом заявленный контракт батареи размер не оговаривает: `Backend/CasProbe.h:8-24` — «The probe validates the backend preconditions required by a writable content-addressed pool: … 2. Conditional-create and conditional-overwrite are enforced», и `:26-28` — «a backend that does not pass the battery MUST NOT be used to coordinate a content-addressed pool». Это и есть overclaim: сертифицируется один транспортный путь, а протокол ездит по двум.

**(2) Условная запись реально уходит через multipart — ПОДТВЕРЖДЕНО, и путь горячий.**

`Backend/CasObjectStorageBackend.cpp:914-931` (`putIfAbsentStream`): `WriteSettings ws = conditionalWriteSettings(); ws.object_storage_write_if_none_match = "*";` → `object_storage->writeObject(...)` → `WriteBufferFromS3`. Тела блобов пишутся именно этим синком: `Pool/CasPartWriteTxn.cpp:611-620` (`sink = store->backend().putIfAbsentStream(key)`, затем `writeString(buildHeader(), out)` + `copyData(*source.open(), out)`), т.е. КАЖДЫЙ файл части, не попавший в дедуп, — это conditional create произвольного размера. Тело > первого буфера/порога → `writeMultipartUpload` (`src/IO/WriteBufferFromS3.cpp:393-405`), и precondition переносится на завершение: `src/IO/WriteBufferFromS3.cpp:651-656`
```
if (!write_settings.object_storage_write_if_none_match.empty())
    req.SetIfNoneMatch(write_settings.object_storage_write_if_none_match);
if (!write_settings.object_storage_write_if_match.empty())
    req.SetIfMatch(write_settings.object_storage_write_if_match);
```
Т.е. на ETag-диалекте multipart для условной записи не запрещён — запрет адресный и только для generation-диалекта: `Backend/CasObjectStorageBackend.cpp:804-814` (`tokenProducingWriteSettings`: `if (native_token_type == TokenType::Generation) { ws.s3_force_single_part_upload = true; ws.s3_single_part_upload_max_bytes_override = token_producing_single_put_cap; }`) → `src/IO/WriteBufferFromS3.cpp:407-416` бросает `NOT_IMPLEMENTED` ДО первого multipart-запроса. Комментарий на `src/IO/WriteBufferFromS3.cpp:657-661` это прямо фиксирует: «a conditional write on a generation-token store never reaches this request … Marking it anyway lets Task 4's native adapter reject a conditional CompleteMultipartUpload outright if that invariant is ever violated» — то есть для ETag-хранилищ conditional CMU ЯВЛЯЕТСЯ штатным путём, по построению.

Второй такой же примитив — S3-native staging promote: `Backend/CasObjectStorageBackend.cpp:1083` (`copyObjectConditional(..., conditionalWriteSettings())`), вызывается из `Pool/CasPartWriteTxn.cpp:604-606`. Для больших объектов `copyS3File` тоже вешает precondition на `CompleteMultipartUpload` — см. контракт `src/IO/S3/copyS3File.h:35-40` («both the single-operation `CopyObject` request and, for large objects, the multipart `CompleteMultipartUpload` request») и `src/IO/S3/copyS3File.cpp:722-723`, `:770-774`, `:838-840`. Проверяет же его `probeConditionalCopy` только на 26-байтном объекте (п. 1). Т.о. пробел одинаков для ОБЕИХ write-once CREATE-примитив, а находка называет только одну.

**(3) Достижимость и радиус — уточнение к находке.**

- AWS S3 поддерживает `If-None-Match` на `CompleteMultipartUpload` (с ноября 2024), так что на референсном бэкенде поведение корректно.
- GCS — единственное измеренное хранилище, игнорирующее preconditions на завершении multipart (`docs/superpowers/cas/BACKLOG.md:429-434`: «Google's own XML API documentation says "Preconditions are not supported in the requests", and it was measured independently on 2026-07-03 (`0a3bc2f1fc6`)»), и оно как раз fail-closed: forced single-PUT + cap, а тело выше cap даёт громкий `NOT_IMPLEMENTED`. Это ровно то, что предписывает правило «loud failure ≫ silent corruption».
- Остаётся класс «сторонние S3-совместимые хранилища, честно исполняющие `If-None-Match` на `PutObject` и молча игнорирующие его на `CompleteMultipartUpload`». Такое хранилище пройдёт батарею и будет считаться сертифицированным. Ни одного такого хранилища на HEAD не измерено (в отличие от GCS), т.е. эксплуатируемость — гипотеза, а не наблюдение; поэтому не P1.
- Радиус на таком хранилище неоднороден: для тел блобов ключ контент-адресный, и потерянный precondition даёт перезапись байт-идентичного тела с новым `incarnation_tag`/токеном — это ломает атрибуцию инкарнации (writer получит `Done` вместо `PreconditionFailed` и пойдёт по ветке «я создал», см. `Pool/CasPartWriteTxn.cpp:643-660` вместо adopt/resurrect на `:665-700`), но не подменяет содержимое; повреждение здесь — не байты, а состояние протокола (в частности displacement condemned-инкарнации минует reconcile-путь `:680-700`). Для write-once объектов управляющего плана (record'ы ref-лога, снапшоты, part-манифесты) потеря precondition — это уже clobber чужого объекта, и молчаливый; но эти тела пишутся строкой через `nativeConditionalPut` (`Backend/CasObjectStorageBackend.cpp:202-218`) и превышают `max_single_part_upload_size` (по умолчанию 32 MiB) только на экстремальных размерах (снапшот каталога в сотни тысяч записей), т.е. это узкий хвост, а не типовой случай.

**(4) Что реально надо сделать (и это отсутствует на HEAD).** Батарею нужно расширить одним шагом, который экзерсайзит именно multipart-путь: conditional create тела размером выше `max_single_part_upload_size` (или с `s3_min_upload_part_size`, приведённым вниз, чтобы шаг оставался дешёвым), повторно на занятый ключ → ожидание `PreconditionFailed`; и то же для `probeConditionalCopy`. Альтернатива в духе уже принятого GCS-решения — не сертифицировать, а сузить: форсить single-part для условных записей на любом не сертифицированном store и падать громко выше cap. Ни того, ни другого в коде нет; `Pool/CasPool.cpp:457-481` вызывает батарею как есть.

**BACKLOG/история.** Прямого анкера нет: по `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` все совпадения по «multipart» — про GCS (`BACKLOG.md:419-457` `{#gcs-conditional-overwrite-rethink}`, `BACKLOG/formats-and-storage.md:23` «[GCS production-grade follow-ups]») и про emulated-resurrect в RAM (`{#emulated-resurrect-spill-to-disk}`); ETag-хранилища там не рассматриваются, что подтверждает: пробел не отслеживается. Ближайший по КЛАССУ отслеживаемый пункт — `BACKLOG/formats-and-storage.md:24` `{#list-consistency-real-s3}`: «Add a LIST-consistency probe in `Cas::Probe` before LIST-derived discovery is trusted on a given store», т.е. та же идея «passport'а для конкретного store» (и `BACKLOG/gc.md:22` про сертификацию LIST). Разумно оформить новый пункт того же вида: «conditional-write passport must cover the multipart finalize path». По истории: `git log -S "SetIfNoneMatch" -- src/IO/WriteBufferFromS3.cpp` даёт только `1f6b7ba9c5c` — строка на CMU не менялась после введения, ничего это не закрывало.

## CAS-032 — Форма кода подтверждена — идентичность пула нигде не привязана к endpoint/бакету, но вредное следствие требует операторской ошибки (запись в CRR-приёмник / двунаправленная репликация поверх префикса), которая нарушает уже подразумеваемое, но НЕ задокументированное требование «префикс принадлежит только CAS»; фикс = требование в docs + необязательная advisory-запись endpoint в mount-lease. (частично, P2) {#cas-032}

## 1. Что реально в коде на HEAD (якорь устарел)

Якорь `CA/Pool/CasPoolMeta.cpp:100-104`, `:111-119` устарел: файл лежит в
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPoolMeta.cpp` (168 строк),
релевантная функция — `PoolMeta::createOrValidate` (`Pool/CasPoolMeta.cpp:108-168`).

Утверждение «ничто не связывает идентичность пула с endpoint или регионом» — **держится**:

- Состав `PoolMeta` — `Formats/CasPoolMetaFormat.h:24-32`: `pool_id`, `blob_header_len`,
  `gc_shards`, `min_reader_generation`, `algos_used`. Ни endpoint, ни бакета, ни региона, ни
  какого-либо URL-производного поля нет.
- `pool_id` минтится из RNG (`Pool/CasPoolMeta.cpp:24-29` `mintPoolId`), т.е. это чистая
  случайная метка, не производная от адреса хранилища.
- Единственная проверка идентичности на открытии/переоткрытии — сравнение `pool_id` и
  `blob_header_len` со «своими»: `Pool/CasPool.cpp:124-128` (`fresh.pool_id == expected_pool_id`,
  иначе `Vanished(replaced)` с текстом «data root replaced by a foreign pool (pool_id …)»),
  диагностика — `Pool/CasPool.cpp:85`, `:324`. Для **CRR-копии** `pool_id` побитово тот же, так
  что этот детектор по построению молчит. Это ровно то, о чём говорит находка.
- Аренда монтирования тоже не несёт адреса хранилища: `Formats/CasServerRootFormats.h:47-58`
  (`MountLease`: `server_uuid`, `writer_epoch`, `hostname`, `pid`, `started_at_ms`, `seq`,
  `expires_at_ms`, `min_active`, `gc_fenced`) — `hostname` относится к серверу, не к endpoint.
- В интроспекции тоже нет: `grep -n "endpoint|bucket" src/Storages/System/StorageSystemContentAddressedMounts.cpp`
  даёт пусто; `CasLifecycleSnapshot` (`ContentAddressedMetadataStorage.h:109-117`) содержит
  `pool_id`/`server_root_id` и никакого адреса.
- Счётчик эпох писателя (`ServerEpoch`, `Formats/CasServerRootFormats.h:38-41`, «stored value is
  the next epoch to allocate») хранится **только в самом бакете**; локального floor нет
  (сознательное решение, ср. «recovery cold-LIST trusted, no local floor»). То есть на отстающей
  копии счётчик отстаёт и `writer_epoch` может быть переиспользован.

## 2. Где находка перегибает

Находка помечена `INTEGRITY` с формулировкой «conditional writes are then made against a bucket
whose contents lag». Проверка достижимости:

(а) **Read-only монтирование реплики «для read scale-out».** Отстающая копия даёт устаревший
ref-log/каталог — это чтение старого состояния, а не порча. Если манифест уже прилетел, а тело
блоба ещё нет, чтение падает **громко** (отсутствующий объект — ошибка, не подстановка). На
relink-пути это тоже fail-closed: `ContentAddressedExchange.h:148-151` — «When the local manifest
cannot be committed — for example a required blob body is absent at precommit — adoption publishes
nothing and the caller falls back to fetching the part bytes». Так что «read scale-out на реплике»
= stale reads + громкие ошибки, не тихая порча.

(б) **Writable монтирование CRR-приёмника / двунаправленная репликация поверх префикса.** Вот
здесь следствие реально: репликация перезаписывает объекты **без** условных предикатов, поэтому
она способна откатить/подменить объекты, на исключительность которых опирается протокол (аренда
монтирования, `gc/state`, тела манифестов под ключом `writer_epoch:build_sequence:ordinal`), а
отставший `ServerEpoch` позволяет заново выдать уже использованную эпоху. Это путь к тихому
расхождению. Но он требует операторского действия, которое ломает базовую предпосылку CAS:
«`pool_prefix` is exclusively CAS-owned» (`Pool/CasPoolMeta.cpp:139-146`, комментарий
BOOTSTRAP GATE). Двунаправленно реплицируемый префикс — это по сути второй, неусловный писатель в
префиксе; ни один детектор идентичности пула эту конфигурацию поймать не может, только запрет в
требованиях.

Формулировка «мониторование реплики неотличимо от primary» **верна**; формулировка impact
«conditional writes are made against a lagging bucket» верна только для (б).

Отдельно: предложенное «привязать идентичность к endpoint» противоречит явно зафиксированному
решению в коде — `ContentAddressedExchange.h:156-158`: «Two replicas may relink iff equal —
endpoint/prefix string-matching is unsafe (false positives => mis-relink)». То есть заменять
`pool_id` строкой endpoint нельзя; допустимо только **дополнительное advisory-поле** (лог/аренда),
которое не участвует в решениях протокола.

## 3. Что нашлось в BACKLOG и истории

- Прямого покрытия нет: `grep -niE "cross-region|CRR|failover|replica bucket|bucket replicat|pool identity"`
  по `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` — пусто. Анкер не выдаю.
- Пользовательская документация требований к бакету (`docs/en/antalya/cas/bucket-requirements.md:18-31`)
  перечисляет read-after-write, условные create/overwrite, exact-token delete, ranged GET, LIST,
  «no versioning / no delete markers», `TOKEN ⟹ CONTENT` — и **не содержит** ни требования
  «один пул = один бакет/префикс», ни запрета репликации бакета/префикса, ни предупреждения о
  монтировании CRR-приёмника. Это и есть настоящая, проверяемая дыра.
  Замечу: соседняя триажная работа уже открывала «bucket requirements docs gap»
  (`a41d42ffe45 ca: 2031-triage — CAS-012 adjudicated; backlog: bucket requirements docs gap + Glacier classification`),
  так что правку логично класть тем же абзацем требований.
- В истории попыток привязать идентичность к адресу не было: `git log -S` по `endpoint` в
  CAS-дереве не даёт коммита, вводившего/удалявшего такую привязку; единственное релевантное
  зафиксированное решение — цитата выше из `ContentAddressedExchange.h`.

## 4. Что реально осталось (предложение)

1. (основное, дёшево) В `docs/en/antalya/cas/bucket-requirements.md` добавить явное требование:
   пул живёт в ровно одном бакете+префиксе; репликация бакета/префикса (CRR/двунаправленная) поверх
   `pool_prefix` запрещена; приёмник репликации нельзя монтировать writable, а read-only
   монтирование копии даёт устаревшие данные и не поддерживается как «read scale-out».
2. (необязательно, advisory) Писать endpoint+bucket в `MountLease`/`system.cas_mounts` **только как
   диагностику** — чтобы «пул тот же, адрес другой» было видно оператору и в логе, без участия в
   решениях протокола (иначе нарушим решение `ContentAddressedExchange.h:156-158`).

Тихая порча возможна, но только за пределами контракта, который надо просто записать; кода,
который бы ломался в поддерживаемой конфигурации, здесь нет. Отсюда P2 (трекать после релиза),
PRE-RELEASE = нет.

## CAS-033 — Ворота действительно пул-широкие и без границы — это осознанный fail-closed выбор («лучше не рекламировать, чем удалить лишнее»), позиция в коде подтверждается; но «нет сигнала оператору» — фактически неверно (WARNING с разбором причин, ProfileEvent, `suppressed` в phase-метриках, `pending_reclaim`/`wedged_namespace_count`), а гранулярность уже трекается в BACKLOG. (by-design, P2) {#cas-033}

## 1. Форма кода на HEAD (якоря устарели)

Якорь `CA/Gc/CasGc.cpp:2063-2064` устарел. На HEAD ворота вычисляются в
`Gc/CasGc.cpp:3066-3068`:

    result.suppress_destructive =
        !report.anomalies.empty() || !carried_holds.empty() || frontier_incomplete;

с большим блоком-обоснованием `/// ==== THE DESTRUCTIVE GATE ====` на `Gc/CasGc.cpp:3027-3067`
(«Computed ONCE, here, from three independent terms, and consulted at every destructive site of the
round»). Термы: аномалии раунда, перенесённые holds (`result.carriedHolds()`, `:3039`),
неполный frontier (`result.frontier_complete`, `:3062-3065`).

Точки потребления (старые номера `:609-610, :791-792, :799-800, :830-832, :862-863, :893-898` не
соответствуют HEAD; фактические):
`Gc/CasGc.cpp:750` (чтение из `folded`), `:799` (redelete блобов — `kNothingToDelete`),
`:1052` (курсор manifest-sweep), `:1071` (`pruneSupersededGenerations`), `:1134` (parent seal runs),
`:1188` (manifest cleanup), `:1221` (namespace janitor), `:1228` (`cleanupRefObjects`),
плюс `Gc/CasBlobInDegree.cpp:471`, `:480` (merge/graduation) и `Gc/CasGcShardPlan.cpp:52-61`.
Декларация — `Gc/CasGc.h:585` (`bool suppress_destructive = false;`), контракт — `Gc/CasGc.h:862`
(«a suppressed round prunes NOTHING and leaves …»).

Итого: «одно скалярное OR по всему пулу, потребляемое во всех деструктивных точках» — **держится**.
Позиция «fail-closed под неопределённостью» тоже держится и явно записана в коде:
`Gc/CasGc.cpp:790-796` («it stops the delete I/O, and nothing else … Do not read this as a licence
to relax the merge-side gate»), а также в пользовательской документации —
`docs/en/antalya/cas/architecture/garbage-collection.md:108-112` («Under suppression there is no
graduation, no redelete, and no ref or namespace deletion; condemnation and sparing continue,
because both are non-destructive»). Предзаполненный вердикт one-liner'а (Filimonov: «prefer
fail-closed safety under GC uncertainty; reclaim may stall») соответствует коду на HEAD — не
пересматриваю.

## 2. Что в находке неверно

**(а) «no operator signal» — фактически неверно.** На каждый подавленный раунд есть:

- `ProfileEvents::increment(ProfileEvents::CASGCClampSuppressedPasses)` — `Gc/CasGc.cpp:3071`;
- **WARNING** с полным разбором причин — `Gc/CasGc.cpp:3103-3111`: «CAS GC fold: destructive work
  SUPPRESSED this pass — N anomaly(ies), M held namespace(s), frontier … (X of Y namespace(s)
  proven…)», причём с явным `deficit_note` («per-cause breakdown of the unproven namespaces»,
  `:3096-3100`) и `catalog_empty_note` (`:3089-3094`). Уровень сознательно разделён на
  WARNING/INFO: WARNING только когда есть «per-round cause, которую оператор может отработать»
  (`Gc/CasGc.cpp:3073-3087`);
- `suppressed`-метрика в phase-строках `system.cas_gc_log`: `Gc/CasGc.cpp:1166`, `:1212`, `:1229`,
  `:1269`, `:3259`;
- события `GcFoldClamp` в `system.cas_log` с `namespace_`/`reason` для каждой причины
  (`Gc/CasGc.cpp:2298-2301`, `:2574-2580`, `:2683-2686`);
- накапливающийся `pending_reclaim` и `wedged_namespace_count` в `system.cas_mounts`
  (`Gc/CasGcScheduler.h:123-130`), документированные как операторский запрос —
  `docs/en/antalya/cas/operations/monitoring.md:92-98`;
- документированная наблюдаемость GC, включая сам счётчик —
  `docs/en/antalya/cas/architecture/garbage-collection.md:230-241`.

**(б) «одна `lifeless` запись» — не тот механизм.** `lifeless` — понятие **fsck**, а не GC:
`Tools/CasFsck.h:148` (`uint64_t lifeless_keys`), `Tools/CasFsck.cpp:460-470`,
`Tools/CasFsck.h:210` (в списке hard findings). В `suppress_destructive` `lifeless` не участвует
вообще (grep по CAS-дереву: только `Tools/CasFsck.*`). Пример в находке подобран неверно.

**(в) «одна нераскодированная строка» — частично не тот механизм.** Аномалии **janitor'а** (то,
что похоже на «undecodable row» при листинге namespace-объектов —
`Gc/CasNamespaceJanitor.cpp:74`, `:80`, `:99`, `:117`) в ворота НЕ попадают: janitor запускается
уже после вычисления ворот (`Gc/CasGc.cpp:1221`), его аномалии только логируются WARNING'ом
(`Gc/CasGc.cpp:479-480`) и считаются как `CASGCNamespaceCleanupLeaks`; описание счётчика прямо
говорит «remains leak-only: it neither suppresses destructive GC nor blocks catalog lifecycle
progress» (`src/Common/ProfileEvents.cpp:886`). Ворота закрывают только **fold-аномалии**
(`Gc/CasGc.cpp:1749`, `:2297`, `:2381`, `:2393`, `:2427`, `:2573`, `:2682`).

**(г) «no bound» — верно, и это намеренно.** Никакого «через N подавленных раундов всё равно
удаляем» нет и быть не должно; в BACKLOG есть отдельно зафиксированное решение того же класса
(`docs/superpowers/cas/BACKLOG.md:79-81`: «The user decision was that the knob must not exist at
all — a cap here converts a bounded burst into a permanent leak, which is worse than no cap»).

**(д) «indefinitely» — только для стойких состояний.** Аномалии пересчитываются каждый раунд из
живого состояния, holds едут в seal до устранения. Т.е. подавление длится ровно столько, сколько
живёт причина. Причём часть причин — узкие транзиентные окна нормальной записи:
`Gc/CasGc.cpp:2567-2573` записывает аномалию «fold barrier: live precommit body not yet present
(non-activating)» — то есть параллельный INSERT в момент fold'а способен закрыть ворота на этот
раунд по всему пулу. Это самая сильная реальная версия жалобы (чувствительность рекламации к
конкурентной записи, а не к настоящим поломкам), но она не «бесконечна»: на следующем раунде тело
уже видно, а провабельно мёртвый precommit пропускается без клампа (`Gc/CasGc.cpp:2540-2566`,
`CASGCDeadPrecommitSkipped`). Что реклама на практике идёт — подтверждено измерением:
`docs/superpowers/cas/BACKLOG.md:244-245` (944 155 `DiskS3DeleteObjects` за 90-минутный
деструктивный soak).

## 3. Покрытие в BACKLOG

Гранулярность ворот уже трекается, и дважды:

- `docs/superpowers/cas/BACKLOG/docs-and-cleanup.md:36-38` — пункт 3 списка refactoring-кандидатов:
  «The destructive gate collapses per-namespace facts into a pool-wide boolean. `suppress_destructive`
  is a single scalar OR over every namespace's anomalies/holds/frontier state, so one un-cataloged
  namespace stalls reclamation for the whole pool. Wants to be per-namespace.»
- `docs/superpowers/cas/BACKLOG/gc.md:65` `{#ckpt-damage-no-repair-path}` — резидуал (a): «a held
  namespace still shuts the ROUND-WIDE destructive gate, so one unrepaired `_ckpt` stops all
  reclamation pool-wide until repaired — full isolation needs Stage B's per-namespace destructive
  gate»; там же резидуал (b) — у повреждённого `_ckpt` нет пути ремонта, т.е. единственный
  известный **стойкий** сценарий «навсегда» уже описан вместе с операторским действием.

Как основной анкер беру `{#ckpt-damage-no-repair-path}` — он единственный с якорем и именно про
пул-широкость ворот.

## 4. Побочная находка (не часть CAS-033, но зафиксирую)

`UniversePolicy::kDefault = Authoritative` на HEAD (`Gc/CasGc.h:63`), флип пришёл коммитом
`58fd482a800` («ca: draft — gc universe authoritative flip (UNVERIFIED-DRAFT, no runs)», 2026-08-03).
При этом:
- описание счётчика в `src/Common/ProfileEvents.cpp:803` устарело — оно всё ещё утверждает «In the
  current stage this is EVERY folding round by construction … the namespace universe is not yet
  knowable», что было верно для Stage A `StageA_Suppressed`, а теперь вводит оператора в
  заблуждение;
- `docs/superpowers/cas/BACKLOG/gc.md:61` `{#stage-b-7b-sequencing}` объявляет этот флип ЖЁСТКИМ
  ограничением: «`UniversePolicy::kDefault` must not flip before Stage B's incarnation-keyed cursors
  land» — стоит проверить, закрыто ли ограничение (HEAD-коммит `684161dcc03` «cas: prove namespace
  absence per-row, not by whole-catalog stillness» выглядит релевантным), либо снять запись.
Оба пункта — мелкие, но их стоит оформить отдельными backlog-правками, а не внутри CAS-033.

## 5. Что реально осталось

Ничего пред-релизного: ворота — сознательный fail-closed, сигнал оператору есть и документирован.
Остаётся уже трекаемая работа Stage B — сделать ворота **пер-namespace** ({#ckpt-damage-no-repair-path}
резидуал (a) + `BACKLOG/docs-and-cleanup.md:36`), плюс путь ремонта повреждённого `_ckpt`
(резидуал (b)), без которого один объект способен остановить рекламацию по всему пулу до ручной
операции. Отсюда P2, PRE-RELEASE = нет.

## CAS-028 — Ключи блобов на HEAD действительно неподсолённые пул-глобальные хеши контента — это и есть суть CAS-дедупа (не пересматриваем); dedup-оракул через `system.cas_log` требует явного гранта (не доступен непривилегированному пользователю), а вот отсутствие crypto-shred-примитива подтверждается и в операторской документации не написано ни одной строкой. (by-design, P3) {#cas-028}

## (a) Код по-прежнему соответствует позиции «ключ = хеш контента, пул-глобально, без соли»

Подтверждено дословно.

- `Formats/CasLayout.cpp:34-37`: `String Layout::blobKey(const BlobRef & ref) const { return shardedKey("blobs/" + String(blobHashAlgoName(ref.algo)), blobHexOf(ref)); }` — в ключ входят только имя алгоритма и hex дайджеста.
- `Formats/CasLayout.h:471-477`: `shardedKey` = `prefix + "/" + ns + "/" + id.substr(0,2) + "/" + id`, где `ns` здесь — литерал `"blobs/<algo>"`, а не пространство имён таблицы. Итоговая форма — `POOL/blobs/<algo>/S/<hex>` (задокументирована в `Formats/CasLayout.h:102-107`).
- Ни namespace, ни `server_root_id`, ни UUID таблицы, ни tenant в ключ блоба не входят — в отличие от манифестов/рефов/state, которые как раз живут под `POOL/cas/ns/...` (`Formats/CasLayout.h:85-95`). То есть per-namespace изоляция в лейауте ЕСТЬ, и она сознательно не распространена на тела блобов.
- Соли/keyed-hash в коде нет вообще: `grep -i salt` по всему каталогу `ContentAddressed/` не даёт ни одного попадания. Алгоритм выбирается из трёх неключеванных (`Primitives/CasBlobDigest.cpp:6-17`: `ch128`/`xxh3`/`sha256`), фиксируется на пул при создании.
- Место, на которое ссылается находка, сохранилось (номер строки устарел): `Pool/CasPartWriteTxn.cpp:186` — `const String key = store->layout().blobKey(req.ref);`, и рядом комментарий `:182-185`: «`logical_ref` is the blob identity end-to-end».
- Позиция «по замыслу» зафиксирована в пользовательской документации: `docs/en/antalya/cas/architecture/blob-protocol.md:79-84` («Two blobs are the same object **if and only if** they hash to the same digest… identity is *proven* by hash equality»), и там же прямо разобран многотенантный threat-model в части коллизий: `:94-99` — «`cityhash128` is not cryptographically collision-resistant. A pool shared across mutually untrusted writers should run `sha256` — CAS enforces no policy choice here; the operator picks the threat model via `blob_hash`».

Вывод по (a): код полностью соответствует пред-заполненному вердикту. Дедуп по контенту не пересматриваем.

Одно уточнение к формулировке находки: «unreclaimed deleted content stays addressable by anyone who can guess the digest» — **последствие недостижимо на уровне SQL**. Ни одной поверхности «прочитать блоб по хешу» в продукте нет: чтение всегда идёт ref → манифест → перечисленные в манифесте `BlobRef` (`Pool/CasManifestReader.cpp:157` — единственный способ получить ключ блоба на read-пути), а `SYSTEM CAS`-команды (`src/Parsers/ASTSystemQuery.h:150-156`: `CAS_GC_RUN`, `CAS_GC_REBUILD`, `CAS_DROP_POOL_MEMBER`, `CAS_FSCK`, `CAS_FORGET`, `CAS_GC_STOP`, `CAS_GC_START`) не содержат «прочитать объект». «Угадав дайджест», атакующий получает доступ к байтам только имея креды к бакету — а с кредами к бакету он и так читает всё. То есть это не эскалация, а свойство «дайджест — не секрет».

## (b) Dedup-оракул: что `system.cas_log` реально отдаёт непривилегированному пользователю

**Ничего — таблица закрыта грантом, как и любой другой системный лог.**

- Таблица называется именно `system.cas_log` (имя находки верное): `src/Interpreters/SystemLog.h:21` — `M(ContentAddressedLog, cas_log, ...)`.
- Гейт доступа: `src/Access/AccessControl.cpp:300` — `setSelectFromSystemDatabaseRequiresGrant(config_.getBool("access_control_improvements.select_from_system_db_requires_grant", true))`, и поставляемый конфиг это включает: `programs/server/config.xml:881` (`<select_from_system_db_requires_grant>true</select_from_system_db_requires_grant>`), то же в `programs/server/embedded.xml:78` и в тестовом `tests/config/config.d/enable_access_control_improvements.xml:6`.
- Список неявно доступных системных таблиц — `src/Access/ContextAccess.cpp:199-231` (`always_accessible_tables`): `one`, `contributors`, `licenses`, `formats`, `privileges`, `databases`, `tables`, `columns`, `settings`, … . `cas_log` в нём **отсутствует**, как и `query_log`. Значит для чтения нужен явный `GRANT SELECT ON system.cas_log`.

Что видит тот, у кого грант ЕСТЬ (то есть оператор, а не арендатор):

- Колонки: `src/Interpreters/ContentAddressedLog.cpp:24-40` — в частности `object_hash` («Content hash (lowercase hex) of the object»), `namespace`, `ref_name`, `token`, `outcome`, `reason`, `query_id`.
- Строка дедупа существует и говорит прямым текстом, что контент уже был: `Pool/CasPartWriteTxn.cpp:429-441` — `e.type = CasEventType::BlobReuseAdopt` (рендерится как `blob_reuse_adopt`, `Primitives/CasEvent.cpp:20`), `e.outcome = "adopt"`, `e.reason = "observed token not condemned (meta point-read); adopted the live incarnation (no bytes moved)"`.
- Но **чужого владельца строка не называет**: билдер заполняет только `object_hash`/`token`/`round`/`outcome`/`reason`; `namespace_`/`ref_name` в этом событии не выставляются, а сток их не додумывает (`ContentAddressedMetadataStorage.cpp:571-596` — прямое копирование полей события, без обогащения). То есть один ряд говорит «этот хеш уже кто-то занял», а не «его занял tenant X».
- Реальная (и более существенная) экспозиция — не оракул, а то, что лог **пул-/сервер-глобальный**: с одним грантом читаются строки ВСЕХ namespace одного диска, вместе с `query_id` (`ContentAddressedMetadataStorage.cpp:593`), что позволяет джойном с `system.query_log` атрибутировать `blob_put` конкретному пользователю и потом сопоставить с чужим `blob_reuse_adopt` по `object_hash`. Это ровно та же модель, что у `system.query_log`/`system.blob_storage_log`: гранулярности «свои строки» у системных логов в ClickHouse нет вообще.

И главное для оценки серьёзности: **оракул не является привилегией `cas_log`**. Он доступен каждому инсертящему пользователю без всяких грантов через ProfileEvents собственного запроса (нативный протокол отдаёт их клиенту, плюс `system.query_log.ProfileEvents`): `src/Common/ProfileEvents.cpp:760` — `CASBlobPutDeduplicated` («Number of CAS blob deduplicating PUT requests. Grows when uploads reuse existing content»), `:766` — `CASBlobDeduplicationCacheHit`; инкременты — `Backend/CasInstrumentedBackend.cpp:87` и `Pool/CasPartWriteTxn.cpp:214`. Плюс сам латентностный сигнал (загруженные/не загруженные байты) неустраним при любом content-addressed дедупе. Так что закрывать `cas_log` как «фикс оракула» бессмысленно — оракул неотделим от дедупа, а `cas_log` и так закрыт.

Вывод по (b): **не баг**. Таблица access-controlled по умолчанию поставляемым конфигом; одна строка не выдаёт владельца совпавшего контента; сам dedup-оракул существует, но он — прямое следствие пул-глобального дедупа (то есть та часть находки, которую мы не пересматриваем), и наблюдаем без `cas_log`.

## (c) Crypto-shred: примитива нет, и это НЕ написано для оператора

Примитива «удалить эти байты везде» в продукте нет — подтверждено:

- Удаление всегда идёт через in-degree-фолд GC, а не через адресное стирание: `docs/en/antalya/cas/architecture/garbage-collection.md:135` — «In-degree is a set of source edges, not a refcount. A blob becomes a candidate when its edge set [becomes empty]»; `docs/en/antalya/cas/quick-start.md:138` — «get reclaimed once nothing references them anymore».
- Самая близкая административная операция явно оговаривает, что общий контент она не трогает: `docs/en/antalya/cas/operations/migration.md:172-175` — `SYSTEM CAS DROP POOL MEMBER` «emits ordinary ref-edge deltas rather than a GC transition: it does not synchronously reclaim shared blob content, it only makes the now-unreferenced blobs eligible for an ordinary GC round to reclaim later».
- Из семи `SYSTEM CAS`-команд (`src/Parsers/ASTSystemQuery.h:150-156`) ни одна не является стиранием по субъекту: `CAS_FORGET` — это забывание объекта в GC-состоянии (снятие 404-HEAD-штурма), а не удаление тела.
- Единственный сайт удаления тела блоба — `Gc/CasGc.cpp:802` (`backend.deleteExact(layout.blobKey(entry.ref), entry.token)`), и он достижим только для записи с уже нулевым in-degree. То есть если тот же контент есть у другой таблицы/реплики в пуле, удалить его физически нельзя никакой командой — что и утверждает находка.

BACKLOG: тема отслеживается, но именно как «желательная фича», а не как задокументированное ограничение:

- `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:25` — «**[B14] expedited / GDPR right-to-erasure delete** — DESIRABLE — Under GC lock, confirm no live ref, then delete bypassing the two-phase graduation delay; no layout change». Обратите внимание: B14 УСКОРЯЕТ удаление уже нессылаемого блоба («confirm no live ref»), то есть **не решает** заявленный случай — блоб, который всё ещё ссылается чужой манифест. Постановка задачи в бэклоге уже́ неявно принимает «shared ⇒ не стираем».
- `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:26` — «**[B17] encryption-at-rest × content-addressing** — DESIRABLE — Dedup scope per-encryption-key; local to key/hash derivation». Это ровно «half» находки про соль/шред: per-key дедуп-скоуп даёт и криптографический шред (уничтожил ключ — байты недоступны), и заодно устраняет кросс-тенантный оракул. Отслежено, не сделано.

Чего **нет** — и это единственный реальный остаток находки:

1. В `docs/en/antalya/cas/` нет ни слова `GDPR`, `shred`, `erasure`, «right to be forgotten» (проверено grep'ом по `docs/en/antalya/cas/*.md` и `*/*.md` — попаданий ноль). Оператор нигде не может прочитать, что (i) `DROP TABLE`/`ALTER DELETE` на CAS-диске не гарантирует физического исчезновения байтов, пока их разделяет другая таблица/реплика/бэкап-namespace, и (ii) продукт не предоставляет операции адресного стирания. Про механику сказано (`quick-start.md:138`, `migration.md:174`), про **последствие для регуляторного стирания** — нет. Это и есть то, что стоит закрыть.
2. Симметрично: в `blob-protocol.md:{#dedup-identity}` многотенантный threat-model рассмотрен только в части коллизионной стойкости (`:94-99`), но не в части «дедуп — это канал подтверждения контента и барьер для стирания». Одна врезка на 3-4 предложения там же закрывает и (b), и (c) для читателя.

## Что реально осталось

Только документационная работа + уже отслеженный B17:

- **DOC (это и есть остаток):** в `docs/en/antalya/cas/bucket-requirements.md` или в `blob-protocol.md#dedup-identity` (плюс ссылка из `operations/troubleshooting.md`) написать прямо: дедуп пул-глобален; удаление освобождает байты только когда исчезла последняя ссылка во всём пуле, включая другие серверы и `shadow/`-namespace бэкапов; примитива «стереть этот контент везде» нет; арендаторы, которым нужен изолированный шред, должны получать отдельный пул (отдельный `pool_prefix`/бакет), а не отдельный namespace. Заодно — что дайджест не является секретом и что факт дедупа наблюдаем через ProfileEvents.
- **Отслежено, не блокер:** B17 (per-encryption-key дедуп-скоуп) закрывает и соль, и шред, и оракул архитектурно; B14 стоит переформулировать, чтобы он не читался как «GDPR-стирание сделано» — он про ускорение уже нессылаемого.
- Кода менять не нужно: (a) — by-design, (b) — не баг (грант по умолчанию требуется, ряд не выдаёт владельца), (c) — ограничение, требующее не примитива, а честной строчки в документации.

P3 и не pre-release: единственный дефект — пробел в операторской документации; никакой достижимой утечки данных или потери данных за находкой нет.

## CAS-029 — Центральное утверждение находки ложно: versioning-предусловие проверяется НЕ конфигурационной GCS-проверкой, а обязательным поведенческим mount-пробом (`created_delete_marker`), который отбивает любой versioned-бакет на AWS и любом S3-совместимом сторе; реально остались три узких остатка — fail-open GCS-проверки, включение versioning ПОСЛЕ монтирования (там `LOGICAL_ERROR` вместо нормальной ошибки, и только на пути тела блоба) и `skip_access_check`. (частично, P3) {#cas-029}

## Что находка описала верно (форма кода)

1. **Диалект действительно объявляется конфигурацией, а не выводится.** `src/IO/S3/Client.cpp:988-991`: `bool Client::supportsGcsNativeConditionalRequests() const { const auto http_client = Poco::toLower(client_configuration.http_client); return http_client == "gcp_oauth" || http_client == "gcs_hmac"; }` — это чистое чтение настройки. CAS берёт его один-в-один: `Backend/CasObjectStorageBackend.cpp:49` — `if (mode == Mode::Native && object_storage->conditionalOpsUseGenerationTokens()) native_token_type = TokenType::Generation;`, а `S3ObjectStorage::conditionalOpsUseGenerationTokens` (`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:535-538`) просто проксирует этот метод клиента.
   Уточнение: «never detected» — преувеличение. `provider_type` в клиенте КАК РАЗ выводится из endpoint'а: `src/IO/S3/Client.cpp:263-272` (`deduceProviderType`: `.amazonaws.com` → AWS, `storage.googleapis.com` → GCS), присваивается в `:298`. Просто CAS-диалект привязан не к `provider_type`, а к `http_client`, потому что generation-диалект требует именно проводки GCS-нативного HTTP-клиента. И это **задокументировано для оператора**: `docs/en/antalya/cas/bucket-requirements.md:38` — «Generation-token dialect: … opted into via `http_client = gcs_hmac` or `gcp_oauth`». (Мелкий док-баг: `bucket-requirements.md:55` обещает описание «how the backend **detects** which one a given endpoint speaks» — это слово следует заменить на «is configured with».)

2. **`checkPoolPreconditions` действительно выполняется только для Native+Generation и fail-open.** `Backend/CasObjectStorageBackend.cpp:56-59`: `if (mode != Mode::Native || native_token_type != TokenType::Generation) return;`; `:60-76` — при `!versioned.has_value()` только `LOG_WARNING` и `return`, причём с явным обоснованием в комментарии `:61-67` («We proceed on the ASSUMPTION that versioning is off rather than fail-closing the mount on an unknown»). Бросок — только при подтверждённом `true` (`:78-84`, `NOT_IMPLEMENTED`). Источник `nullopt` — `S3ObjectStorage::isBucketVersioningEnabled` (`S3ObjectStorage.cpp:540-551`: неуспешный `GetBucketVersioning` → `std::nullopt`) либо база `IObjectStorage`, не переопределяющая метод. Поведение прямо закреплено тестами: `src/Disks/tests/gtest_cas_backend_generation.cpp:114-122` (`CheckPoolPreconditionsProceedsOnUnknownVersioning`) и `:125-131` (`CheckPoolPreconditionsNoOpOnEtagDialect`).

3. **`LOGICAL_ERROR` после уже выполненного удаления — есть.** `Gc/CasGc.cpp:802-806`: сначала `DeleteOutcome del = backend.deleteExact(layout.blobKey(entry.ref), entry.token);`, затем `if (del.created_delete_marker) throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS gc: delete of blob {} created a delete marker — versioning is enabled on the pool (mis-provisioned; the capability probe must reject this)", ...)`.

## Что в находке ФАКТИЧЕСКИ НЕВЕРНО — и это её ядро

«On AWS S3 and every S3-compatible store the versioning check is skipped entirely» — **неверно**. `checkPoolPreconditions` — это не единственная и не главная проверка; главная — обязательный поведенческий проб, провайдер-агностичный и работающий именно через тот сигнал, который находка объявила «unverifiable by construction».

- `Backend/CasProbe.cpp:209-224` (шаг 8 батареи): после успешного `deleteExact` с верным токеном — `if (d.created_delete_marker) throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CasProbe: deleteExact succeeded but created a versioning delete marker — the bucket has object VERSIONING enabled, and a content-addressed pool cannot run on a versioned bucket… This is NOT ignorable and has no override. Use a bucket where versioning was NEVER enabled — note that merely SUSPENDING versioning is not enough…")`. Это строго сильнее конфигурационной проверки: проверяется не заявленное состояние бакета, а фактическое поведение DELETE.
- Проб выполняется на КАЖДОМ writable-монтировании: `Pool/CasPool.cpp:381` (`if (!config.read_only)`) → `:457` (`if (!config.skip_access_check)`) → `:467` `runCapabilityProbe(*backend, config.pool_prefix + "/_probe/" + u128ToHex(probe_uid));`. Read-only монтирование пробу не делает, но и не удаляет ничего (GC на read-only отбивается `checkNotReadOnly`, `ContentAddressedMetadataStorage.cpp:602`). Сам `checkPoolPreconditions` тоже вызывается изнутри пробы (`Backend/CasProbe.cpp:47`), то есть это ДОПОЛНИТЕЛЬНЫЙ ранний шаг, а не замена.
- Сигнал реален на настоящем S3: `S3ObjectStorage.cpp:515-516` — `if (outcome.IsSuccess()) return {ConditionalRemoveOutcome::Removed, outcome.GetResult().GetDeleteMarker()};` (то есть заголовок `x-amz-delete-marker` ответа `DeleteObject`), и он доносится до CAS: `Backend/CasObjectStorageBackend.cpp:1016` — `d.created_delete_marker = result.created_delete_marker;`.
- Проб покрыт тестом: `src/Disks/tests/gtest_cas_probe.cpp:61` — `b->setSimulateDeleteMarkers(true); // versioning enabled on the prefix`, и он обязан отбить пул (`Backend/CasInMemoryBackend.cpp:251,282`, `CasInMemoryBackend.h:129`).
- Требование задокументировано для оператора: `docs/en/antalya/cas/bucket-requirements.md:13-14` («a capability probe that runs at every writable mount and fails closed»), `:26` (строка таблицы «No versioning / no delete markers | probed by `runCapabilityProbe`; `created_delete_marker` on `DeleteOutcome`»), `:29-31`.

История: поведенческая проверка старая, а не «была и сломалась» — `git log -S"created a versioning delete marker"` даёт единственный коммит `f6b8aa8eb8a` «CA core M-C1: fail-closed capability probe (enforced delete + versioning-off checks)». GCS-специфичная надстройка добавлена ПОЗЖЕ и именно потому, что на GCS поведенческий сигнал недоступен: `git log -S"isBucketVersioningEnabled"` → `101597fc585` («CAS/GCS: IObjectStorage token-kind + bucket-versioning capability surface»), `f46ea3db3e5` («CAS/GCS: probe store-preconditions hook; fail closed on a versioned GCS bucket»), `373f7becbf9`. Комментарий в коде говорит то же самое явно: `Backend/CasObjectStorageBackend.cpp:53-55` — «Only the Native, generation-dialect (GCS) combination has anything to check». То есть предыдущий вердикт CAS-011 («CAS checks at startup that versioning is off») **остаётся верным**, а классификация «was-fixed / still-present» в one-liner'е — результат чтения одной проверки в отрыве от пробы.

Класс `DATA-LOSS` тоже завышен: delete-marker означает, что байты СОХРАНЕНЫ (архивированы как noncurrent version), а не потеряны. Ущерб — неосвобождаемое хранилище (счёт) + остановка reclaim + аварийный выход GC-раунда. Потери данных за находкой нет.

## Что реально осталось (три узких остатка)

1. **Fail-open GCS-проверки** (`CasObjectStorageBackend.cpp:60-76`) — подтверждается, но это осознанное решение с предупреждением в лог и без поведенческой альтернативы: на GCS удаление живой версии при включённом versioning возвращает успех без delete-marker'а, поэтому проб её увидеть НЕ может, а падать на «не смогли проверить» (типовой случай — просто нет права `storage.buckets.get`) означало бы отказ монтировать нормальный бакет. Худший исход — GC перестаёт освобождать место (стоимость), а не порча. Здесь fail-open обоснован; максимум — поднять уровень до однократного `LOG_ERROR` и вывести факт «versioning unverified» в `system.cas_mounts`, чтобы оператор видел это не только в текстовом логе.
2. **Versioning включён ПОСЛЕ успешного монтирования** (это единственный триггер находки, который не отбивается пробой) — тогда первый же GC-раунд, дошедший до redelete тела блоба, ловит `Gc/CasGc.cpp:803-806`. Два дефекта в этом обработчике:
   - **Код ошибки неверен.** `LOGICAL_ERROR` — это утверждение «состояние программы невозможно», а состояние здесь достигается внешним действием оператора (`aws s3api put-bucket-versioning` на живом пуле). Сообщение само это признаёт («mis-provisioned; the capability probe must reject this») — но проб отработал корректно ДО изменения. Должно быть `NOT_IMPLEMENTED`/`BAD_ARGUMENTS` с текстом «versioning was enabled on the bucket after this pool was mounted — disable it and restart the mount», иначе в debug-сборке это assert, а в релизе — строка «Logical error» в логе, которая уводит триаж в поиск бага CAS.
   - **Покрыт только путь тела блоба.** `created_delete_marker` проверяется ровно в одном месте (`grep` по `Gc/` даёт единственное попадание `CasGc.cpp:803`), а остальные destructive-сайты раунда его игнорируют: удаление манифеста `Gc/CasGc.cpp:1193`, номинации `:1243`, generation-prune `:3461`, `:3563`, `:3569`, janitor `Gc/CasNamespaceJanitor.cpp:111`, orphan-sweep `Gc/CasOrphanManifestSweep.cpp:586`. Практически это не меняет исход (объёмный путь — тела блобов, он выстрелит), но детекция асимметрична: раунд, у которого в этот проход нет `redelete`-записей, тихо наплодит delete-marker'ов.
   Заявленная в находке альтернатива «wedges all reclamation» силой не подтверждается как «silently»: раунд валится с исключением и валится снова на каждом следующем раунде — это громко, видно в `system.cas_gc_log`/тексте лога, и совпадает с сценарием из `operations/troubleshooting.md:21` («GC never seems to reclaim space»).
3. **`skip_access_check` полностью выключает пробу** — `Pool/CasPool.cpp:457,469-479`: при `skip_access_check = true` (per-disk настройка, дефолт `false`, `ContentAddressedSettings.cpp:69`: «Skip the boot-time capability probe (start now, fix later)») выполняется только `checkConditionalWriteSingleAttemptSupport`, а versioning не проверяется ни поведенчески, ни на GCS. Комментарий `:476-478` это признаёт и оценивает как «purely environmental (slower GC reclaim, not data loss) and gets re-checked the next time this pool is opened without skip_access_check» — оценка корректная. Отдельного действия не требует, но при описании остатка (2) это второй путь в то же состояние.

## BACKLOG / история

Точного покрытия по versioning в бэклоге нет. Найдено смежное:
- `docs/superpowers/cas/BACKLOG/docs-and-cleanup.md:83-92` {#bucket-requirements-lifecycle-worm-glacier} (остаток прошлой триажи, CAS-012): «The settled position (a CAS pool requires a plain bucket: no lifecycle expiration, **no versioning**, no Object Lock/WORM, no storage-class transitions; CAS cannot detect any of them without admin access) is only half-delivered in the user docs. `bucket-requirements.md:26,29-31` documents versioning only» — то есть versioning как раз ТА часть, которая задокументирована; недостают lifecycle/WORM/Glacier.
- `docs/superpowers/cas/BACKLOG/formats-and-storage.md:23` — «[GCS production-grade follow-ups] … `gcp_oauth` dialect probe validation against live GCS (ADC creds)» — ближайший пункт к остатку (1).
- `docs/superpowers/cas/BACKLOG.md:419` {#gcs-conditional-overwrite-rethink} — про GCS-условную перезапись, не про versioning.

Предлагаемый новый анкор — {#versioning-enabled-after-mount}, объём работы маленький: (а) переклассифицировать `Gc/CasGc.cpp:804` из `LOGICAL_ERROR` в достижимую ошибку с текстом «включили versioning после монтирования» + gtest на `simulate_delete_markers` через реальный раунд (in-memory backend это уже умеет); (б) проверять `created_delete_marker` на остальных destructive-сайтах раунда единым хелпером; (в) заменить «detects» на «is configured with» в `bucket-requirements.md:55` и дописать одну фразу «versioning must never be enabled later on a live pool either».

P3 и не pre-release: заявленной дыры (незащищённый versioned AWS/MinIO/RustFS-бакет) на HEAD нет — она закрыта обязательной поведенческой пробой; остаток сводится к коду ошибки, симметрии детекции и одной формулировке в документации.

## CAS-034 — Формы кода подтверждены (5000 ref-объектов/раунд, одна страница janitor'а на 1000 ключей), но следствие — отложенная утилизация и рост латентности стирания, а не потеря/порча данных; SLA стирания закрыт позицией автора, арифметика «бюджет vs скорость создания» реальна и уже отслеживается в BACKLOG. (частично, P2) {#cas-034}

1. Проверка анкоров (номера строк в финдинге устарели, файлы переехали в подкаталоги).

- `gc_round_ref_cleanup_budget = 5000` — подтверждено: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp:80` («Ref-object cleanup (covered log/snapshot deletes) cap per round (0 = unbounded)»). Анкор финдинга `CA/ContentAddressedSettings.cpp:46` — устаревший номер строки, но настройка та же.
- `gc_interval_sec = 60` — подтверждено: `ContentAddressedSettings.cpp:66`; валидация `>= 1` — `ContentAddressedSettings.cpp:178-181`.
- Бюджет проводится в раунд: `Gc/CasGc.cpp:545` (`round_work_budget.max_ref_cleanup_objects = store->poolConfig().gc_round_ref_cleanup_budget`), потребление — `Gc/CasGc.cpp:3504-3508` и `3516-3520`, предикат — `Gc/CasBlobInDegree.h:287` (`max == 0` ⇒ безлимитно).
- Janitor «одна страница на 1000 ключей за раунд» — подтверждено и даже жёстче, чем в финдинге: размер страницы **захардкожен**, настройки нет — `Gc/CasGc.cpp:470`: `NamespaceJanitor janitor(backend, layout, 1000);`, вызов ровно один раз за раунд (`Gc/CasGc.cpp:1221`, и в suppress-режиме `Gc/CasGc.cpp:710`). Сам проход — `Gc/CasNamespaceJanitor.cpp:9-141`: один `backend.list(layout.namespaceRootPrefix(), cursor, page_budget)` (`:25`), `result.pages = 1` (`:32`), курсор публикуется только если страница «решена» (`:128-139`).
- «Снапшот раз на 256 аппендов» — подтверждено: порог `snapshot_log_count_threshold = 256` / `snapshot_log_bytes_threshold = 1 MiB` (`Pool/CasPool.h:234-235`, `Pool/CasRefProtocol.h:158-159`), триггер — `Pool/CasRefLedger.cpp:3983-3985`.

2. Что в утверждениях финдинга неточно.

- «Every part commit creates two ref objects» — неверно как утверждение об **объектах**. Ref-мутации батчуются: один карвинг батча = одна попытка аппенда = **один** объект `_log` (`Pool/CasRefLedger.cpp:3148`: `prepared.prepared_attempt.key = layout.refLogKey(life, id)`; батч — `Pool/CasRefLedger.cpp:2615` `flushRefBatch`, `2810-2868` карвинг, `CASRefBatchedMutations` на `:3832`). Две ref-операции на коммит партиции (публикация + снятие precommit/tmp) — это две **записи внутри одной транзакции/одного объекта**, а не два объекта. Под нагрузкой батчинг растёт, т.е. скорость создания объектов само-ограничивается латентностью раунд-трипа на namespace, а не линейна по коммитам.
- Потолок утилизации, посчитанный корректно: 5000 объектов / 60 с ≈ 83 удаления ref-объектов в секунду на весь пул. Это действительно **потолок**, и он не зависит от нагрузки. Арифметика верна, но порог высокий: чтобы его перебить, нужен устойчивый темп ≳83 батч-флашей ref-лога в секунду по всему пулу.
- «Debris accumulates without bound» — с оговоркой: работа **не теряется**, она откладывается. `planRefCleanup` каждый раунд заново пересчитывает тот же остаток из долговременного состояния — это прямо задокументировано на месте отсечения (`Gc/CasGc.cpp:3499-3503`: «Exhaustion simply stops the round's cleanup pass here; `planRefCleanup` recomputes the SAME remaining candidates from durable state next round, so nothing here needs its own cursor»). То же в BACKLOG: класс B, «work is deferred, not lost». Это не молчаливая порча и не fail-open: ни одно удаление не выполняется без per-key валидации (HEAD + повторное чтение каталога + повторное чтение `gc/state` перед каждым `deleteExact`, `Gc/CasGc.cpp:3410-3461`).
- «Called unconditionally» и прочие безусловности к CAS-034 не относятся (это CAS-035).

3. Что реально осталось (и почему P2, а не P3).

- Обратная связь, которую финдинг не назвал, но которая делает пункт не косметическим: неутилизированные covered `_log`/`_snap` объекты остаются под `cas/ns/stream/`, а раунд GC делает **полное** перечисление этого префикса с удержанием всех ключей в памяти (`Gc/CasGc.cpp:3771-3811`, `scan.keys.push_back(lk.key)`). Т.е. отставание cleanup напрямую удорожает каждый следующий раунд (LIST + память), что смыкается с CAS-035. Это единственная часть, где «отложено» постепенно деградирует в «дороже», а не остаётся нейтральным.
- Janitor: страница 1000 ключей захардкожена (`Gc/CasGc.cpp:470`), настройки нет, страница берётся по **всему** `namespaceRootPrefix()`, а не по мусору — значит латентность физического стирания после `DROP` — это O(всех namespace-объектов пула) / 1000 за раунд, а не O(мусора). При 1 млн объектов namespace-дерева полный цикл ≈1000 раундов ≈ 16 ч на дефолтной каденции. Janitor при этом единственный удалитель этого класса — подтверждается комментарием в `Tools/CasFsck.cpp:512` («`NamespaceJanitor::runOnePage` (the only real deleter of this debris)»), а `deletePrefixWholesale` (`Gc/CasGc.cpp:3544`, `3656`) чистит только собственные генерации GC (`gc/gen/<g>/`), не namespace-поддеревья.

4. Позиция автора и её соответствие коду (не пересматривается).

Пред-заполненный вердикт («erase SLA is not part of the disk contract; operator can `GC RUN` anytime; GC cadence ~5–10 min; GDPR faster-than-that unlikely») коду соответствует: ручной прогон существует и выполняется независимо от `SYSTEM CAS GC STOP` (`src/Interpreters/InterpreterSystemQuery.cpp:1023-1030`, `:2509 runContentAddressedGcRun`), каденция настраиваемая (`gc_interval_sec`), а бюджет ref-cleanup оператор может снять полностью (`0 = unbounded`, `CasBlobInDegree.h:287`). Единственное, что позицией не покрыто: janitor нельзя ни ускорить настройкой, ни снять — только повторными `GC RUN`.

5. Покрытие в BACKLOG (существующее, новых записей не требуется).

- `docs/superpowers/cas/BACKLOG.md:347` — `{#gc-round-budgets-not-backpressure}`: ровно этот класс («A per-round count cap is not backpressure… If arrival exceeds `budget × rounds/sec`, the deficit is not smoothed, it accumulates»), с покласcовой классификацией; `gc_round_ref_cleanup_budget` отнесён к классу B (`BACKLOG.md:361-367`).
- `docs/superpowers/cas/BACKLOG.md:391` — `{#gc-budgets-need-a-deadline}`: назван настоящий фикс — wall-clock дедлайн раунда плюс курсоры там, где сейчас несут список; прямо сказано, что дедлайна в коде нет нигде.
- `docs/superpowers/cas/BACKLOG/gc.md:140` — `[gc-files-prefix-not-listed]` подтверждает роль janitor'а как реального реклеймера этого класса.
- Замечание по документу (не по коду): `BACKLOG.md:347` в заголовке утверждает «four defaults changed», однако на HEAD `gc_round_graduation_budget`, `gc_round_redelete_budget`, `gc_round_handoff_prefix_wholesale_budget`, `gc_round_outcome_entry_budget` по-прежнему равны 5000 (`ContentAddressedSettings.cpp:76-83`); безлимитным стал только `gc_frontier_probe_budget`, и то не как настройка, а как поле пула (`Pool/CasPool.h:152`, `std::numeric_limits<uint64_t>::max()`). Т.е. текст BACKLOG в этой части опережает код — стоит поправить формулировку при следующем касании файла.

6. Итог. Формы кода — подтверждены. Класс — отложенная утилизация и латентность стирания (громко наблюдаемая, fail-closed по каждому удалению), не потеря и не порча. Половина про SLA стирания закрыта позицией автора и коду соответствует. Реально открыто и уже отслежено: (а) отсутствие временного дедлайна раунда как настоящего механизма backpressure, (б) обратная связь «неубранный ref-мусор дорожает раунд», (в) захардкоженная и ненастраиваемая страница janitor'а с латентностью O(размер namespace-дерева). Всё это post-release scale-работа, не блокер релиза.

## CAS-035 — Подтверждено по всем пунктам, причём пик памяти хуже, чем описано: раунд GC делает полное перечисление `cas/ns/stream/` с удержанием всех ключей в памяти и материализует ВЕСЬ новый edge-run в одной строке в памяти (при дефолтном `gc_shards=1` — целиком по пулу); класс уже полностью отслежен в BACKLOG как O(pool)-per-round. (подтверждено, P2) {#cas-035}

1. Полный LIST с удержанием в памяти, без курсора — ПОДТВЕРЖДЕНО.

`Gc/CasGc.cpp:3771-3811` (`Gc::enumerateRefPrefix`; анкор финдинга `CasGc.cpp:2561-2593` — устаревшие номера): `forEachListedKey(backend, layout.casRefsPrefix(), …)` c `scan.keys.push_back(lk.key)` на **каждый** ключ префикса `cas/ns/stream/`, плюс `scan.logs_by_life[...]` и `max_log_by_life` — то есть в памяти остаётся весь набор ключей ref-дерева пула. Курсора/резюмируемости нет ни в функции, ни у вызывающего. Заголовок это фиксирует как проектное решение: `Gc/CasGc.h:838` («One full enumeration of `cas/ns/stream/`») и `Gc/CasGc.h:815-817` («The pool-wide ref LIST (`enumerateRefPrefix`/`groupRefKeys`) remains the INTRA-namespace hint»).

2. Вызывается до решения о DEFER, в том числе на раундах, которые затем откладываются — ПОДТВЕРЖДЕНО, но следствие частично by-design.

`Gc/CasGc.cpp:642`: `walk_plan.emplace(buildRefWalkPlan(listRefPrefix(state)));`, и только затем `Gc/CasGc.cpp:646` `defer_round = shouldDeferRound(changed, …)`, где `changed = walk_plan->changedRows()` (`Gc/CasGc.cpp:645`). Т.е. сам сигнал изменений **производится из** этого перечисления — вычислить решение о DEFER без LIST текущая схема не может. Это прямо задокументировано на месте: `Gc/CasGc.cpp:628-630` («Its result is retained (rather than discarded once the defer decision is taken) because `fold` regroups the very same keys instead of listing the prefix again. A deferred round simply drops it»). Поэтому формулировку финдинга «called unconditionally, including on rounds it then defers» надо читать как «отложенный раунд всё равно платит полный LIST», а не как «лишний вызов, который можно просто убрать»: убирается он только вместе с внешним change-signal — ровно то, что BACKLOG называет `[Lever B]` («Also provides the global change-signal that would let GC drop the per-round `LIST(cas/refs/)` sweep»).

3. Свёртка O(total pool) и неограниченный пик памяти — ПОДТВЕРЖДЕНО, и сильнее анкора.

- Входная сторона свёртки действительно стримится: `Gc/CasBlobInDegree.cpp:52-60` (`PriorEdgeCursor`) читает предыдущий run посегментно, комментарий в `zeroInDegree` подтверждает «streamed at O(one block) resident memory, never materialized whole» (`Gc/CasBlobInDegree.cpp:700-702`).
- Но **выходная** сторона материализуется целиком: `Gc/CasBlobInDegree.cpp:389-390` `DB::WriteBufferFromOwnString out; SourceEdgeRunWriter writer(out);`, затем `Gc/CasBlobInDegree.cpp:678-681` `out.finalize(); const String run_bytes = out.str();` — в момент PUT в памяти лежат две копии всего run'а шарда. Run содержит по строке NDJSON на каждое **выжившее ребро** пула (`writer.append(SourceEdgeRecord{…})`, `Gc/CasBlobInDegree.cpp:672-676`), т.е. пик = O(все живые (blob, source) ребра / `gc_shards`). Дефолт `gc_shards = 1` (`ContentAddressedSettings.cpp:73`), т.е. по умолчанию делителя нет. Это точное подтверждение «unbounded peak memory» — причём анкор финдинга (`CasBlobInDegree.cpp:484-555`, свёртка/`settleEntry`) указывает не на самое дорогое место.
- Дополнительно неограничены в памяти: `rmr.still_retired` / `rmr.spared` / `rmr.graduated` (векторы, `Gc/CasBlobInDegree.cpp:466-508`) — O(retired), ровно тот класс, который BACKLOG называет «класс A, feedback loop» (`BACKLOG.md:355-359`).

4. `std::vector<BlobDelta> deltas` без бюджета — ПОДТВЕРЖДЕНО, с уточнением класса.

`Gc/CasGc.cpp:1925` `std::vector<BlobDelta> deltas;` (анкор `:1379` устарел), наполняется по завершении каждого лога: `Gc/CasGc.cpp:2528` (`std::vector<BlobDelta> log_deltas` — per-log буфер) и `Gc/CasGc.cpp:2605-2606` (`for (BlobDelta & d : log_deltas) deltas.push_back(std::move(d))`). Бюджета на этот вектор, на GET'ы ref-логов и на декоды манифестов (`foldManifestEdges`, `Gc/CasGc.cpp:1301`, `1356`) действительно нет. Точный класс роста — O(бэклога с последней свёртки), а не O(пула); но именно это и означает вторую половину утверждения финдинга: «a backlog must be absorbed in one round» — резюмируемости нет, весь накопленный бэклог обрабатывается одним раундом. Единственный связанный бюджет по памяти — `rebuild_edge_budget` на пути восстановления (`Gc/CasGc.cpp:4103-4104`), к обычному раунду он не применяется.

5. Триггер «worst after leader loss, cleared hold, object-store outage» — согласуется с уже измеренным поведением: BACKLOG фиксирует натурный переход через границу устойчивости очереди — время раунда 20 с → 1716 с за 6 раундов при росте кандидатов 188 → 20 046 (`BACKLOG/gc.md:70-90`, `{#gc-throughput-collapse-2026-07-25}`).

6. Покрытие в BACKLOG (новых записей не требуется; финдинг — переизложение уже отслеженного).

- `docs/superpowers/cas/BACKLOG/gc.md:15` — `{#gc-scalability}`:
  - `[Lever B] Incremental point-updatable in-degree` — прямо содержит замер «GC round is O(pool objects) — 87ms@400 parts → 93s@10k tables → 398s@100k parts» и цель убрать per-round `LIST(cas/refs/)`;
  - `[gc-snapshot-log-structured-runs]` — «hot-pool snapshot rewrite is O(edges) per pass … the single dominant remaining byte cost», фикс = log-structured O(delta) runs + компакция (это ровно пункт 3 выше);
  - `[gc-frontier-one-list]` — стоимость обнаружения и параллельный обход, с оговоркой про LIST-trust (см. также settled-вердикт `docs/superpowers/cas/2026-08-03-list-trust-verdict.md`).
- `BACKLOG.md:391` — `{#gc-budgets-need-a-deadline}`: «A GC round has no time deadline anywhere in the code» — это же и есть отсутствующая резюмируемость, о которой говорит финдинг.
- Замечание к пред-заполненному вердикту («contradicts prev CAS-057's 🟡 "single LIST, no fan-out" mitigation»): противоречия нет — обе части верны одновременно. LIST действительно один на раунд и без fan-out'а (`Gc/CasGc.cpp:642`, `Gc/CasGc.cpp:628-630`), но он полный, с удержанием всех ключей и без курсора. «Один» смягчает число запросов, но не стоимость O(пул) и не пик памяти.

7. Классификация серьёзности. Это деградация производительности/памяти, а не потеря или порча данных: свёртка fail-closed на каждом сомнительном шаге (кламп на отсутствующем теле манифеста — `Gc/CasGc.cpp:2520-2540`; проверка чек-суммы run'а перед решением об удалении — `Gc/CasBlobInDegree.cpp:715-717`; переучёт in-degree на месте удаления — `Gc/CasBlobInDegree.cpp:422-437`). Реальные риски: (а) раунд, переросший TTL лизы, отфенсивается (класс, который уже лечили в P3.1 — см. `BACKLOG.md:393-399`), (б) пик памяти edge-run'а на большом пуле при `gc_shards=1`. Ни то, ни другое не блокирует релиз при заявленном масштабе, но и то, и другое обязано быть в capacity-модели.

8. Итог: подтверждено полностью (с более точными анкорами, чем в финдинге), уже отслежено тремя пунктами `{#gc-scalability}` и `{#gc-budgets-need-a-deadline}`; новой информации по сравнению с BACKLOG финдинг не даёт, кроме указания на настоящее место пика памяти (`out.str()` в `foldDeltasIntoGeneration`), которое стоит дописать в `[gc-snapshot-log-structured-runs]`.

## CAS-042 — Форма кода описана верно (одна глобальная генерация как min-reader, `changePoints` не читается на декоде), но это осознанная pre-release политика recreate-only; следствия про «тихое стирание полей» и про `Roster` недостижимы. (by-design, P2) {#cas-042}

Устаревшие якоря. Финдинг ссылается на снапшот `CA/Formats/CasFormat.*`; на HEAD файлы лежат в
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/` (переезд коммитом
592b9b83568). Номера строк устарели: `G_BUILD` не `CasFormat.h:10`, а `CasFormat.h:60`;
`currentWriterVersion`/`currentCompatibilityVersion`/`checkCompatibility` не `CasFormat.cpp:82-93`, а
`CasFormat.cpp:98-116`; «противоречащая таблица :19-75» — это на HEAD два разных объекта:
change-points `CasFormat.cpp:22-62` и таблица трейтов `CasFormat.cpp:139-174`.

Что ПОДТВЕРЖДАЕТСЯ на HEAD (форма кода).

1. Одна глобальная генерация штампуется как min-reader любого объекта.
   `CasFormat.cpp:103-108`: `currentCompatibilityVersion` возвращает `G_BUILD` безусловно, с
   комментарием «Until roster-based write-down is implemented, every object carries the current build
   as its compatibility floor». `CasFormat.cpp:98-101`: `currentWriterVersion` — то же.
   `CasFormat.h:60`: `constexpr uint32_t G_BUILD = 9`. То есть бамп генерации действительно поднимает
   пол чтения у ВСЕХ классов, включая не менявшиеся. Утверждение верное.

2. `changePoints` заполняется и не читается на пути декода.
   Единственные не-тестовые вхождения — определение `CasFormat.cpp:66-96` и объявление
   `CasFormat.h:161`; все прочие — тесты (`src/Disks/tests/gtest_cas_format.cpp:23,41,56`,
   `gtest_cas_text_format.cpp:47`, `gtest_cas_gc_maintenance_state_format.cpp:30`). Гейт на декоде —
   только `v > G_BUILD` (`CasFormat.cpp:110-116`, вызывается из `CasTextFormat.cpp:328` внутри
   `expectHeaderLine`). Это прямо задокументировано и в самом дереве:
   `src/Disks/tests/gtest_cas_format.cpp:36` — «nothing consults `changePoints` at decode time yet»,
   и `gtest_cas_text_format.cpp:245-247`. То есть реестр — задел под будущий per-class пол.

3. «Ничто не связывает изменение формата с бампом `G_BUILD`» — верно: связь только дисциплинарная
   (комментарий-политика `CasFormat.h:13-16` и `Formats/README.md:52-53`), никакой машинной проверки
   нет. Тесты фиксируют лишь согласованность уже записанных change-points, а не факт бампа.

Что НЕ подтверждается (реальная форма кода + недостижимое следствие — типовая ошибка этого аудита).

4. «Толерантные декодеры выбрасывают неизвестные ключи, а read-modify-write циклы их затирают,
   тихо стирая поля более новой ноды». Форма RMW есть: `Pool/CasPoolMeta.cpp:87-101` —
   `decodePoolMeta` → копия структуры → `encodePoolMeta(next)` → `casPut`, а `PoolMeta` толерантен
   (`CasFormat.cpp:143`, `KeyStrictness::Tolerant`, `CasPoolMetaFormat.cpp:160` `r.skipUnknown(key)`).
   Но объект более НОВОЙ генерации до толерантного тела не доходит: `expectHeaderLine`
   (`CasTextFormat.cpp:320-329`) вызывает `checkCompatibility` ДО чтения тела, и `v > G_BUILD` даёт
   `UNKNOWN_FORMAT_VERSION`. Плюс критические ключи `!...` отвергаются даже толерантными форматами
   (`CasTextFormat.cpp:249-251`). Значит «тихое стирание» требует, чтобы новая сборка добавила поле
   БЕЗ бампа генерации и без `!`-префикса, т.е. нарушения той самой политики из п.3. Это риск
   дисциплины, а не дефект HEAD, и он fail-closed, а не silent, ровно там, где политика соблюдена.

5. «`FormatId::Roster` зарегистрирован в `changePoints` без строки трейтов, поэтому обращение к нему
   бросает `LOGICAL_ERROR`». Факт верен: `CasFormat.cpp:83` (ветка `Roster` → `BASELINE`), а
   `traitsFor` (`CasFormat.cpp:177-183`) действительно бросает `LOGICAL_ERROR`, что и задокументировано
   в `CasFormat.h:192-193` как «reserved and has no codec or traits row yet». Но следствие
   недостижимо: `Roster` не пишется, не читается и не мапится ни на один `type` — во всём `src/`
   `FormatId::Roster` встречается ровно в `CasFormat.cpp:83`, `CasFormat.h:107` и в ТЕСТАХ
   (`gtest_cas_text_format.cpp:74,97` — там намеренный `EXPECT_THROW`/`EXPECT_DEATH`, т.е. поведение
   зафиксировано как ожидаемое). Динамический путь `traitsForType` (`CasFormat.cpp:185-191`) вернуть
   `Roster` не может: для незарегистрированного `type` он отдаёт `nullptr`. Итого — зарезервированное
   значение enum с громким assert, а не баг.

6. Trigger «прочитать объект более старой сборкой; или смешанные генерации в одном пуле» на HEAD
   fail-closed на МОНТИРОВАНИИ, а не в бизнес-логике: любой mount декодирует `_pool_meta`
   (`Pool/CasPoolMeta.cpp:126`, `Pool/CasPool.cpp:118,234`), а `decodePoolMeta` держит гейт в ОБЕ
   стороны — назад `CasPoolMetaFormat.cpp:111-117` (`header.v < kCommittedRefFrontierGeneration` →
   `UNKNOWN_FORMAT_VERSION`, «recreate the pool; CAS is pre-release: there is no in-place migration») и
   вперёд через `expectHeaderLine`/`checkCompatibility`, плюс отдельный пол
   `CasPoolMetaFormat.cpp:174-177` по `min_reader_generation`. Так как оба пола равны `G_BUILD = 9`,
   сборка с другим `G_BUILD` пул просто не смонтирует. Смешанная генерация в одном пуле сегодня
   невозможна по конструкции.

BACKLOG и история. Остаток покрыт двумя якорями:
`docs/superpowers/cas/BACKLOG/formats-and-storage.md:74` {#cas-format-version-floor} — ровно про то,
что `checkCompatibility` не применяет per-class пол из `changePoints`, с предложенной формой фикса
(`changePoints(id).front().generation` как backward-пол);
`docs/superpowers/cas/BACKLOG/operability-and-introspection.md:19` (секция {#operability}, пункт
«B180 / format-freeze») — GATE: «durable roster + `max_content_addressable_pool_format`
setting/rollout machinery not built (Part IV)», т.е. отсутствующий write-down-to-floor, который и
делает бамп генерации точечным, а не всеобщим. Дополнительно `BACKLOG/formats-and-storage.md:83`
(наименование `format_version`/`compatibility_version`) — косметика. Позиция пользователя
(«needs attention later; not a blocker; model may be wrong») коду соответствует: pre-release формат
несёт нулевые persisted-обязательства, все бампы 4-9 объявлены recreate-only (`CasFormat.h:25-59`),
и никакая часть находки не даёт тихой порчи данных — все отказы громкие
(`UNKNOWN_FORMAT_VERSION`/`LOGICAL_ERROR`).

Что реально осталось: (а) per-class пол на декоде (уже трекается {#cas-format-version-floor});
(б) roster + write-down-to-floor, чтобы бамп не аннулировал неизменившиеся классы (уже трекается
B180 под {#operability}); (в) отсутствие машинной привязки «изменил кодек → обнови change-points и
`G_BUILD`» — это часть того же B180-гейта, отдельного пункта не заводил. Новых нетрекаемых остатков
нет; часть про `Roster` фикса не требует.

## CAS-043 — Узость catch подтверждена (`CORRUPTED_DATA` only, а гейт версии и критический ключ дают `UNKNOWN_FORMAT_VERSION`), но перекос генераций в одном пуле сегодня невозможен: relink предлагается только внутри одного смонтированного пула, а mount держит точный гейт генерации — остаётся однострочное упрочнение. (частично, P3) {#cas-043}

Устаревшие якоря. `CA/ContentAddressedMetadataStorage.cpp:1610-1619` на HEAD — это
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:2259-2272`
(переезд каталогов коммитом 592b9b83568, сам guard родом из `784c698bb40` «CA GC B7 (1/3)»).
`CasFormat.cpp:90` → `CasFormat.cpp:110-116`. `DataPartsExchange.cpp:1182-1184` → вызов
`relinkPartToDisk` теперь `DataPartsExchange.cpp:944`; «:793-799» → ветка relink на приёмнике
`DataPartsExchange.cpp:889-950`.

ПОДТВЕРЖДАЕТСЯ (форма кода).

1. Фильтр catch действительно только по `CORRUPTED_DATA`:
   `ContentAddressedMetadataStorage.cpp:2259-2272` — `decodePartManifest` в `try`, затем
   `if (e.code() != ErrorCodes::CORRUPTED_DATA) throw;` и лишь иначе
   `return CaRelinkPrepare::MechanismFallbackAllowed`.

2. Ошибка генерации — это НЕ `CORRUPTED_DATA`: `decodePartManifest`
   (`Formats/CasPartManifestFormat.cpp:129`) начинается с `expectHeaderLine(in, FormatId::PartManifest)`,
   тот вызывает `checkCompatibility` (`Formats/CasTextFormat.cpp:328`), который при `v > G_BUILD` бросает
   `UNKNOWN_FORMAT_VERSION` (`Formats/CasFormat.cpp:110-116`). Второй источник того же кода —
   критический ключ `!...`: `Formats/CasTextFormat.cpp:249-251` (`skipUnknown`). Оба — ровно те сигналы
   «эта сборка не умеет читать манифест отправителя», которые формат задуман эмитировать, и оба
   пролетают мимо fallback: исключение уходит из `prepareRelink` наверх через `relinkPartToDisk`
   (`DataPartsExchange.cpp:944`) и валит fetch целиком, вместо перезапроса байтами
   (`fall_back_to_byte_fetch`, `DataPartsExchange.cpp:907-914`).

3. Верно и то, что версия репликационного протокола ничего не говорит о генерации CAS:
   `DataPartsExchange.cpp:103,108` — `REPLICATION_PROTOCOL_VERSION_WITH_CA_RELINK = 10`,
   `..._WITH_CA_CONFIRM = 11`; это чисто wire-возможности.

НЕ ПОДТВЕРЖДАЕТСЯ (следствие недостижимо на HEAD) — заявленный trigger «fetch между двумя нодами с
разным `G_BUILD`».

4. Relink предлагается ТОЛЬКО при совпадении пула: `DataPartsExchange.cpp:926-932` — если
   `!chosen_ca || chosen_ca->getPoolUUID() != advertised_pool_uuid`, идёт байтовый fetch. То есть обе
   стороны обязаны иметь смонтированным ОДИН И ТОТ ЖЕ пул.

5. Смонтировать один пул двумя сборками с разным `G_BUILD` нельзя: mount всегда декодирует
   `_pool_meta` (`Pool/CasPoolMeta.cpp:126`, `Pool/CasPool.cpp:118,234`), а `decodePoolMeta` держит
   точный гейт — назад `Formats/CasPoolMetaFormat.cpp:111-117` (`header.v < kCommittedRefFrontierGeneration`
   → `UNKNOWN_FORMAT_VERSION`, «recreate the pool»), вперёд `checkCompatibility` в `expectHeaderLine`,
   плюс `Formats/CasPoolMetaFormat.cpp:174-177` по `min_reader_generation` (который админ-путь поднимает
   до `G_BUILD`, `Pool/CasPoolMeta.cpp:90,152`). Так как backward-пол `kCommittedRefFrontierGeneration`
   и `G_BUILD` оба равны 9 (`Formats/CasFormat.h:60,91`), допускается ровно генерация 9. Перекос
   генераций внутри пула — конфигурация, которую валидация монтирования не принимает.

6. Wire-перекос отдельно уже обработан и НЕ через это исключение: неизвестное значение cookie
   `cas_relink` (`DataPartsExchange.cpp:916-923`) явным образом деградирует в байтовый fetch с
   комментарием про rolling upgrade. Так что «мешанина сборок» покрыта, просто в другом месте.

7. Формулировка однострочника «was-fixed / still-present» некорректна: предыдущая адъюдикация
   (CAS-209, «fail-closed publish-nothing → byte-fetch fallback; format bumps caught by the manifest's
   own compatibility check») коду не противоречит — проверка совместимости действительно ловит бамп,
   просто громко (fail-closed), а не деградацией. Никакой регрессии/откатанного фикса в истории этого
   guard нет: с `784c698bb40` он всегда был `CORRUPTED_DATA`-only.

Единственный живой путь и его цена. Приёмник читает `sender_manifest_bytes` из сети
(`DataPartsExchange.cpp:936-938`) без предварительного контроля целостности заголовка (payload_digest
проверяется позже и внутри тела), поэтому искажение поля `v` в проводе даст `UNKNOWN_FORMAT_VERSION`
и жёсткое падение fetch вместо деградации. Это громкий fail-closed отказ, который самолечится
повторной попыткой очереди репликации, — не порча данных и не потеря части. Отсюда P3.

Что осталось и чем не покрыто. В `BACKLOG.md`/`BACKLOG/*.md` ни `UNKNOWN_FORMAT_VERSION`, ни узость
этого catch не трекались (проверено grep по обоим); ближайший смежный якорь —
`BACKLOG/formats-and-storage.md:74` {#cas-format-version-floor}, но он про другой конец гейта
(отсутствие backward-пола), и `BACKLOG/operability-and-introspection.md:19` (B180 / format-freeze) —
про будущую rolling-upgrade машинерию. Поэтому добавил новую секцию (не коммичена) в
`docs/superpowers/cas/BACKLOG/replication.md` с якорем
{#relink-fallback-unknown-format-version}: принять `UNKNOWN_FORMAT_VERSION` наравне с
`CORRUPTED_DATA` в этом catch — ровно как уже сделано в `Pool/CasRefLedger.cpp:177` (там оба кода
означают «чужой объект») — и сделать это до первого релиза, который допустит пул со смешанными
генерациями, потому что после этого узкий catch превратит деградируемый fetch в жёсткий стопор
репликации.

## CAS-038 — Из семи подпретензий пять факту на HEAD не соответствуют или имеют безопасное значение по умолчанию; реально остались только необязательный `oc` в журнале исходов GC и отсутствие проверки line cap на стороне записи `gc/state` — и то и другое косметика. (частично, P3) {#cas-038}

Замечание по якорям: все пути в находке даны в снапшотном виде `CA/Formats/...`; на HEAD это
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/...`, номера строк из находки
устарели (в частности `CasFoldSealFormat.cpp:294-305`/`:286` теперь `:455-540`). Все цитаты ниже — по HEAD
(`684161dcc03`, ветка `cas-gc-rebuild`).

1) `decodeMountLease` требует только `su`/`we`, а `eat`/`ma`/`fen` дефолтятся — ФОРМА ВЕРНА,
СЛЕДСТВИЕ ПЕРЕВЁРНУТО. Код: `Formats/CasServerRootFormats.cpp:147-148` (`saw_su`/`saw_we`),
`:171-172` (единственный обязательный набор), дефолты структуры — `CasServerRootFormats.h:55-57`
(`expires_at_ms = 0`, `min_active = 0`, `gc_fenced = false`).

   а) «truncated или partially written lease» НЕДОСТИЖИМО: тело — единственная строка, читаемая
   `readLine`, которая на отсутствии терминатора бросает `CORRUPTED_DATA` («truncated object (line
   without terminator)», `Formats/CasTextFormat.cpp:286-287`). Любой обрыв записи (единственное окно —
   локальный/emulated бэкенд; на S3 PUT атомарен) отрезается ДО конца строки, значит объект не
   декодируется вообще, а не декодируется с пропущенными полями. Остаётся только сценарий
   «подложили руками», т.е. запись в бакет — а это уже за границей модели доверия.

   б) Даже в подложенном виде дефолты — это САМОЕ БЕЗОПАСНОЕ, а не «least-safe», направление.
   Отбор аренды принципиально не делается по настенным часам: `Pool/CasServerRoot.cpp:411-427`
   («never by comparing `expires_at_ms` against `now_ms`») требует для reclaim одного из трёх:
   `gc_fenced` (дефолт `false` — не даёт), маркер чистого прощания `min_active == UINT64_MAX`
   (дефолт `0` — не даёт), либо proven-dead-токен. `fen=false` = «не фенсили», `ma=0` = минимальный
   этаж, при котором уборка не имеет права ничего забирать (`Gc/CasOrphanManifestSweep.cpp:491-493`:
   eligible только при `min_active == UINT64_MAX` или `min_active > build_sequence`). То есть
   «expired, unfenced» из находки не даёт ни захвата аренды, ни удаления.

   в) Единственная точка, где `expires_at_ms` читается напрямую —
   `Pool/CasServerRoot.cpp:236` (`live = !surviving.gc_fenced && surviving.expires_at_ms > now_ms`),
   путь `EpochMintPolicy::DecommissionRecovery`. Там же, в комментарии `:228-235`, разобрано, почему
   ошибочное чтение безопасно: минтимая эпоха по построению отличается от эпохи выжившего
   (`:245`: `max(1, surviving.writer_epoch + 1)`, а `we` — обязательное поле), и следующий за этим
   `claimMount` применяет свой сильный гейт. Вердикт по (1): not-a-bug.

2) `CasGcStateFormat.cpp:50-63` — ФАКТИЧЕСКИ НЕВЕРНО. Якорь показывает только маппинг полей;
обязательность и нижняя граница стоят строкой ниже: `Formats/CasGcStateFormat.cpp:64-65`
(«CAS gc/state: missing gcs», с комментарием `:62-63` «Do NOT silently keep the struct default (1)»)
и `:66-67` («gc_shards must be >= 1»). Сторона записи тоже фейлится закрыто: `:21-22`. Ни одного
дефолта «в наименее безопасную сторону» здесь нет. Хартбит рядом (`:111-112`) требует и `by`, и `seq`.

3) `CasBlobMetaFormat.cpp:66-81` / «`{"st":"condemned"}` декодируется как condemn round 0» — форма
верна (`Formats/CasBlobMetaFormat.cpp:87-88` требует только `st`; `cr`/`sz` опциональны), но
следствия нет. Единственный потребитель декодированного `cr` — `Gc/CasGc.cpp:129-137`
(`writeCondemnedMeta`), и он ветвится ИСКЛЮЧИТЕЛЬНО на `state`: уже-`Condemned` мету он оставляет в
покое, «rather than clobbering a possibly-newer condemn_round» (`Gc/CasGc.cpp:121-122`) — то есть
прочитанное значение `cr` вообще не используется как число. Решение о градации к удалению принимается
по `condemn_round` из shard-строки in-degree-таблицы, а не из `_meta`:
`Gc/CasBlobInDegree.cpp:418`, `:480` (`e.condemn_round < current_round`). Писатель ветвится на
`state` (`Pool/CasPartWriteTxn.cpp:372`, `:681`). Вердикт: not-a-bug.

4) `CasFoldSealFormat.cpp:294-305` «no required-field or junk check» — ФАКТИЧЕСКИ НЕВЕРНО на HEAD.
Junk-проверка: `Formats/CasFoldSealFormat.cpp:526-527` («junk after record»). Обязательные поля:
`rfl` — `:412-414` (нулевой/отсутствующий life id), `:419-420` (`cls` обязателен, с комментарием
именно про «absent read as 0 — a claim about a fold, not the absence of one»), `:435-457` (полная
грамматика hold), `:462-472` (cleanup evidence); `btr` — `:495-497`; `cnd` — `:515-517`. Плюс
трейлер-счётчик (`:364-366`), запрет байтов после трейлера (`:362-363`), `KeyStrictness::Strict` на
каждой строке (`:349`), запрет повторного life id (`:475-479`). Требования по `btr`/`cnd` пришли
коммитом `2bbcbb18683` «Make CAS shard authority durable» (`git log -S "btr requires key, ck, shard,
and gen"`) — то есть уже существующий бэклог-пункт {#seal-decode-remaining-fields} (docs/superpowers/
cas/BACKLOG/formats-and-storage.md:73), описывающий ровно эту дыру, УСТАРЕЛ и закрыт; я отметил это в
приписанной секции. Единственное, что действительно не требуется в meta-строке, — `g`/`pg`
(`:337-343`), но подмена генерации ловится параметром `expected_generation` (`:367-370`), который
передают все три продакшн-вызова с генерацией.

5) «два из четырёх входов fold seal пропускают структурную валидацию» — форма верна, следствие нет.
Перегрузок две (`Formats/CasFoldSealFormat.h:212` и `:217`) плюс `validateFoldSealForWrite` (`:223`).
Валидирующую 4-аргументную использует единственный путь adoption — `Gc::readFoldSeal`
(`Gc/CasGc.cpp:3685-3686`, с `poolConfig().gc_shards` и `generation`), и запись
(`Gc/CasGc.cpp:3318`). Продакшн-потребители 2-аргументной: `Tools/CasInspect.cpp:614` (рендер),
`Tools/CasFsck.cpp:836` (читает `blob_target_runs`) и `Gc/CasOrphanManifestSweep.cpp:104`. Последний —
единственный «принимающий решения» из трёх, и он берёт из печати только coverage-строки
(`coverageOf`, `Gc/CasOrphanManifestSweep.cpp:108-115`), которые 2-аргументный декодер валидирует
полностью; то, что добавляет 4-аргументный (`validateFoldSealStructure`,
`Formats/CasFoldSealFormat.cpp:100-141`: каноничность ключей run, ≤1 run на шард, тотальность
condemned summary по `gc_shards`), к этому пути отношения не имеет. Для fsck нетолерантный декодер был
бы прямым регрессом (диагностический инструмент должен ДОКЛАДЫВАТЬ повреждение, а не падать).

6) «журнал исходов GC не требует того исхода, ради которого существует» — ПОДТВЕРЖДЕНО, но косметика.
`Formats/CasGcOutcomesFormat.cpp:113-114` требует `ha`/`h`/`tt`, но не `oc` и не `k`; запись без `oc`
декодируется как дефолт структуры `OutcomeKind::Spared` (`Formats/CasGcOutcomesFormat.h:37`), `k` — как
`ObjectKind::Blob` (`:35`). При этом doc-комментарий обещает обратное:
`Formats/CasGcOutcomesFormat.h:53-55` («required record fields ... are checked»), т.е. код и контракт
расходятся. Достижимость: только через byte-adopt чужого/повреждённого журнала
(`Gc/CasGc.cpp:979-987`; недекодируемый — `ABORTED`). Единственный потребитель — счётчики отчёта
раунда (`Gc/CasGc.cpp:988-997`), никаких решений об удалении. Итог — перекошенный счётчик; занесено
как {#outcome-log-oc-not-required}.

7) «кодировщик `gc/state` не проверяет line cap, который проверяет его декодер» — ПОДТВЕРЖДЕНО,
недостижимо. `decodeGcState` читает тело с `line_cap` = 64 KiB
(`Formats/CasGcStateFormat.cpp:43` + `Formats/CasFormat.cpp:168`), `encodeGcState` проверяет только
`gc_shards >= 1` (`:21-22`) — в отличие от `encodeFoldSeal`, у которого есть построчный
`checkLineBytes`. Единственное переменной длины поле — `msc` (`:31`), а это ключ страницы LIST
(`Gc/CasOrphanManifestSweep.cpp:905`, `:910`), ограниченный длиной ключа бэкенда (~1 KiB на S3), то есть
на два порядка ниже cap. Если бы это всё же произошло, отказ был бы громкий и fail-closed
(`CORRUPTED_DATA` при следующем чтении, GC заклинивает до ремонта), без тихой порчи. Занесено как
{#gc-state-encode-no-line-cap}.

Итог: класс «декодеры делают критичные поля опциональными и дефолтят их в наименее безопасное
значение» на HEAD не подтверждается — по mount lease дефолты консервативны, по `gc/state` поле
обязательно, по blob meta и fold seal утверждения о коде неверны (в том числе потому, что часть уже
закрыта `2bbcbb18683`). Остаточное — два пункта косметики (6) и (7), оба fail-closed/только-отчётность,
поэтому P3 и не блокируют релиз.

## CAS-039 — Форма описана верно (верхней границы у `gc_shards` нет нигде, а локальное значение из XML молча замещается пуловым), но последствия — громкий fail-closed отказ аллокации и отсутствие warning'а, а не порча данных; отдельно неверно утверждение об отсутствии сравнения — расхождение durable-пары ловится и бросает. (частично, P3) {#cas-039}

Якоря находки — снапшотные (`CA/Pool/...`, `CA/Gc/...`); на HEAD это
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/{Pool,Gc,Formats}/...`, номера строк
сместились (`CasPool.cpp:351-354`/`:547-550` → `:492-497`/`:837-842`; `CasGc.cpp:1294`/`:2140-2147`/
`:2808-2810` → `:1804`/`:3178-3208`/`:4108-4111`). Всё цитируется по HEAD `684161dcc03`.

Происхождение шейпа. Вся описываемая конструкция (durable `gcs` в `_pool_meta` + принятие его в
`PoolConfig`) появилась коммитом `2bbcbb18683` «Make CAS shard authority durable» (2026-08-02): он
добавил `gcs` в кодек (`Formats/CasPoolMetaFormat.cpp:80-81`, `:135-140`), обязательность+ненулевость
(`:164`), поднял floor формата до генерации 8 (`Formats/CasFormat.h:87`) и добавил
`config.gc_shards = meta.gc_shards` в оба открытия пула. То есть находка описывает не старый огрех, а
намеренно введённую «пул — источник истины» модель.

1) «Значение из `_pool_meta` задаёт размеры векторов и границы циклов, проверенное только на `>= 1`» —
ПОДТВЕРЖДЕНО по форме. Проверки на ноль есть на каждой границе и НИ ОДНОЙ верхней:
`ContentAddressedSettings.cpp:178-181` (только `== 0`), `Pool/CasPoolMeta.cpp:115-116` (BAD_ARGUMENTS
на 0 при минте), `Formats/CasPoolMetaFormat.cpp:164` («missing or zero gcs»),
`Formats/CasGcStateFormat.cpp:66-67`, `Gc/CasGcShardPlan.cpp:31-35`,
`Formats/CasFoldSealFormat.cpp:103-104`, `Formats/CasRefCatalogFormat.cpp:333`, `:349`.
Потребители-размеры: `Gc/CasGc.cpp:1804` (`retired_merge.resize(state.gc_shards)`), `:3178`, `:3181`
(`buckets`/`retirement_buckets`), `:4109-4111` (`buckets`/`prior_runs`/`attempt_of` в rebuild).

   Но последствие — громкий fail-closed отказ, а не порча и не выход за границы:
   - индексация всегда в диапазоне: `blobShard` — это `% gc_shards` (`Gc/CasGcShardPlan.h:42`,
     `Gc/CasGcShardPlan.cpp:25`), цикл `for (shard = 0; shard < state.gc_shards; ++shard)`
     (`Gc/CasGc.cpp:3185`), а взятый из печати `run.shard >= gc_shards` отвергается
     (`Formats/CasFoldSealFormat.cpp:113-116`); проход по `retired_merge` идёт по `.size()`
     самого вектора (`Gc/CasGc.cpp:780-782`);
   - при абсурдном значении раунд GC умирает на аллокации (`std::bad_alloc`/`length_error`) — виден в
     логах, ничего не удаляет; тот же класс, что уже зафиксирован как {#sec4-decoder-size-bounds}
     («a pool-write-capable party could place an enormous but validly-framed object»);
   - подложить `_pool_meta` = иметь право записи в бакет, т.е. сценарий вне модели доверия CAS
     (та же граница, что в `feedback_cas_relink_trust_model`).
   Реальный остаток — отсутствие санитарного потолка (дёшево: константа или потолок, выведенный из
   `checkFoldSealReservation`, `Formats/CasRefCatalogFormat.cpp:363-384`). Занесено как
   {#gc-shards-no-upper-bound}.

2) «расхождение с локальным конфигом решается перезаписью локального значения без лога и сравнения» —
РАЗДЕЛЯЕТСЯ НА ДВЕ ЧАСТИ.

   а) Перезапись без сравнения и без лога — ПОДТВЕРЖДЕНО: `Pool/CasPool.cpp:494-497` и `:839-842`
   (`config.gc_shards = meta.gc_shards;`), а `PoolMeta::createOrValidate` на существующем пуле вообще
   не смотрит на переданное значение — `Pool/CasPoolMeta.cpp:124-128` с комментарием «Present => the
   pool is authoritative; ignore the passed config's blob_header_len and run the flag-gated admission
   check». Ровно то же обращение с `blob_header_len`, так что это последовательная модель, а не
   недосмотр; настройка задокументирована как creation-time-only
   (`ContentAddressedSettings.cpp:73`: «creation-time only»). Дефект — чисто операбельность: оператор,
   поправивший `<gc_shards>` на существующем пуле, не получает никакого сигнала. Занесено как
   {#gc-shards-config-override-silent}; починка = один WARNING с двумя значениями.

   б) «без сравнения» как утверждение обо всей системе — НЕВЕРНО: расхождение durable-пары
   ловится и фейлится закрыто. `Gc/CasGc.cpp:4540-4543`:
   `if (current.gc_shards != store->poolConfig().gc_shards) throw Exception(CORRUPTED_DATA, "CAS
   gc/state gc_shards {} disagrees with the pool-authoritative _pool_meta value {}")`. Первичная
   установка — единожды при первом acquire (`Gc/CasGc.cpp:4525-4528`). Кроме того, чтение печати на
   adoption сверяет её структуру именно с пуловым `gc_shards` (`Gc/CasGc.cpp:3686`,
   `Formats/CasFoldSealFormat.cpp:130-141` — condemned summary обязана быть тотальной по шардам), а
   несоответствие тотальности форсирует fail-closed fold (`Gc/CasGc.cpp:3746`, `:3760-3765`,
   `:2988-2998`). Так что «тихого» расхождения между durable-объектами не бывает — тихо теряется
   только незаписанное пожелание из XML.

3) Побочно проверено: генерационный floor не даёт открыть пул без `gcs`
(`Formats/CasPoolMetaFormat.cpp:110-116`, `UNKNOWN_FORMAT_VERSION`, «recreate the pool ... CAS is
pre-release: there is no in-place migration») — то есть «пул без durable gc_shards» не деградирует
молча в 1, а отвергается. `gc_shards > 1` при этом рабочий однопроцессный путь
(`Gc/CasGc.cpp:3169-3208`); параллельный многоворкерный планировщик — отдельный открытый пункт
BACKLOG/gc.md `[distributed gc_shards>1 parallel GC]`, к этой находке не относится.

Итог: подтверждается отсутствие верхней границы и молчаливость adoption'а — оба пункта мелкие
(fail-closed / только-операбельность), опровергается «без сравнения» и снимается класс DECODE/DoS как
release-блокер. P3, до релиза не обязательно.

## CAS-044 — Агрегатный лимит 16 MiB inline-данных на манифест действительно проверяется только в stageManifest и не имеет пути переклассификации в blob — INSERT/мерж падает громко и воспроизводимо; достижимость шире, чем описано в находке. (подтверждено, P2) {#cas-044}

Якоря находки устарели (снапшот до 592b9b83568). На HEAD:
- Константы: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:55-57` — `kMaxManifestInlineBytesTotal = 16ULL << 20`, рядом `kMaxLargestInlineEntryBytes = 1 MiB` и `kMaxManifestEntries`. Находка указывала `CA/Pool/CasPartWriteTxn.cpp:54`.
- Проверка: `CasPartWriteTxn.cpp:813-834` (находка говорила `:514-528`): цикл по entries бросает `LIMIT_EXCEEDED` для per-entry, затем `if (inline_total > kMaxManifestInlineBytesTotal)` — тоже `LIMIT_EXCEEDED`. Комментарий `:812` прямо называет это fail-closed backpressure.

Что подтверждается:
1. «Нет fallback/переклассификации» — ВЕРНО. Per-entry предел имеет spill: inline-кандидат > `INLINE_CAP = 1 MiB` (`ContentAddressedTransaction.cpp:98`) пишется во временный файл и стейджится как обычный blob (`ContentAddressedTransaction.cpp:932-970`, ветка else с комментарием «Safety fallback»). Агрегатного аналога нет: нигде не ведётся текущая сумма inline при стейджинге (единственное вхождение `inline_total` — в `stageManifest`), а `stageManifest` уже не может поменять placement — у него нет доступа к staging-путям. Итог: отказ, а не размещение в blob.
2. «Схемозависимо и воспроизводимо на повторе» — ВЕРНО: набор файлов детерминирован формой схемы/парта, повтор INSERT даст ту же сумму. То же самое ударит по мержу (мерж, порождающий такой парт, не сможет завершиться → накопление партов) и по repoint-пути, который вызывает тот же `stageManifest` (`ContentAddressedTransaction.cpp:356`).
3. «Проверка происходит после того, как всё застейджено» — ЧАСТИЧНО. Файлы действительно уже записаны в локальный scratch/S3-staging, НО `stageManifest` вызывается ДО `uploadPendingBlobs` (`ContentAddressedTransaction.cpp:410-412` и `:356-358`), т.е. до PUT тела манифеста и до загрузки blob'ов. Потери работы меньше, чем утверждает находка.
4. «Виновники — index и marks-данные, масштабирующиеся по числу строк» — ЧАСТИЧНО ВЕРНО, и по другой причине, чем указано. `partFileMustStayBlob` (`ContentAddressedTransaction.cpp:65-73`) держит в blob только `.bin`, `.mrk/.mrk2/.mrk3/.cmrk/.cmrk2/.cmrk3` и точное имя `primary.idx`. Следствия: (а) `.cmrk4` — дефолтное расширение марок компактного парта при `write_marks_for_substreams_in_compact_parts=true` (`src/Storages/MergeTree/MergeTreeIndexGranularityInfo.cpp:96-99`) — НЕ в списке, т.е. марки компактных партов inline-кандидаты; (б) `primary.cidx` (дефолт `compress_primary_key=true`) не в списке; (в) любые skip-index `.idx` не в списке; (г) сравнение по точному имени означает, что `<proj>.proj/primary.idx` проекции тоже inline-кандидат. Файлы проекций лежат в манифесте РОДИТЕЛЬСКОГО парта (маршрутизация `ContentAddressedMetadataStorage.h:459-472`: `Route.file` = `<proj>.proj/<file>` внутри одного ref), поэтому суммирование по проекциям реально. То есть достижимость ШИРЕ, чем «много проекций или skip-индексов»: достаточно ~8-17 файлов чуть меньше 1 MiB (каждый по отдельности не спиллится), например несколько проекций с `primary.cidx` + `.cmrk4` близкими к 1 MiB. Марки wide-партов (`.cmrk2`) при этом действительно остаются blob — в этой части формулировка находки неточна.

Градация: отказ громкий и fail-closed (`LIMIT_EXCEEDED` до PUT тела манифеста и до промоушена ref), без повреждения данных и без частичной публикации — это класс availability/feature-gap, обнаруживаемый на первом же INSERT, а не тихая порча. Поэтому P2, не блокер релиза.

История/BACKLOG: существующего покрытия агрегатного лимита нет. Ближайший родственник — `docs/superpowers/cas/BACKLOG/performance.md` {#part-file-suffix-allowlist-memory} (2031-triage CAS-014), который описывает ту же дыру в allowlist, но явно заявляет «no correctness or manifest-bloat defect (the cap holds)» — там имелся в виду per-entry cap, агрегатный не рассматривался. Тест `src/Disks/tests/gtest_cas_inline_placement.cpp` фиксирует предикат только на именах верхнего уровня и не имеет кейса пути проекции. Константа и проверка не менялись с введения (git log -S `kMaxManifestInlineBytesTotal`).

Реальный остаток: агрегатный лимит без пути демоушена. Добавлена новая секция `docs/superpowers/cas/BACKLOG/formats-and-storage.md` {#manifest-inline-budget-no-spill} (не закоммичено) с owed-фиксом: вести inline-сумму на стейджинге и демотить крупнейшие inline-кандидаты в blob до достижения лимита.

## CAS-045 — Вес записи part-folder кэша всегда равен 256 байт, потому что `Resolved::manifest_size` оба производителя жёстко пишут нулём — байтовый бюджет `part_folder_cache_bytes` и порог oversized-bypass неработоспособны. (подтверждено, P2) {#cas-045}

Якоря устарели, но всё сходится. На HEAD:
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp:136-140`: `estimatedBytes() { return 256 + manifest_size; }` (находка: `CA/Parts/PartFolderAccess.cpp:128-131`).
- `manifest_size` заполняется только из `Cas::Resolved` (`Parts/PartFolderAccess.cpp:69`, поле `Parts/PartFolderAccess.h:123`), объявление `Pool/CasRefProtocol.h:120-128`.
- Оба производителя `Resolved` пишут ноль: `Pool/CasRefLedger.cpp:341-345` (`resolveRef`) и `:373-377` (`listRefs`) — находка указывала `:254-258`, `:273-276`. Больше производителей нет (grep `manifest_size` по всему src даёт только эти два места плюс тесты `gtest_cas_ref_recovery_cas_walk.cpp:703` и `gtest_cas_part_folder_view.cpp:50`, где значение подаётся руками).
- История: поле введено в `7a640e5ac69` («CA GC phase1c: read path over root-local part manifests») уже с `.manifest_size = 0` в обоих местах и никогда не заполнялось (`git log -S ".manifest_size ="` — только рефакторинги переносов). Т.е. это не регрессия, а никогда не подключённая проводка.

Следствие 1 (байтовый бюджет мёртв) — ПОДТВЕРЖДАЕТСЯ. Вес всегда 256, поэтому `part_folder_cache_bytes` (дефолт 64 MiB, `ContentAddressedSettings.cpp:86`, проводка `ContentAddressedMetadataStorage.cpp:804-806`, `CacheBase` создаётся в `Parts/PartFolderAccess.cpp:153-156` c `ViewWeight` = `estimatedBytes`, `Parts/PartFolderAccess.h:372-376`) вырождается в лимит на количество 64 MiB/256 = 262144, что заведомо выше реально действующего `part_folder_cache_max_entries` (дефолт 10000, `ContentAddressedSettings.cpp:87`). Единственная реальная граница — счётчик записей, а каждая запись держит целиком декодированный `PartManifest` вместе со всеми `inline_bytes` (`Formats/CasPartManifestFormat.h:53-64`), т.е. до 16 MiB inline на парт (лимит из CAS-044). Оценка находки «gigabytes» реалистична для широких таблиц: 10000 партов × сотни KiB inline (`checksums.txt`/`serialization.json`/`primary.cidx`) = единицы GiB при заявленном бюджете 64 MiB. `CurrentMetrics::CASPartFolderCacheBytes` (`src/Common/CurrentMetrics.cpp:233`, «Estimated bytes retained by the CA part-folder view cache») сообщает ту же фикцию — 256×entries.

Следствие 2 (oversized-bypass мёртв) — ПОДТВЕРЖДАЕТСЯ. `Parts/PartFolderAccess.cpp:226`: `if (view->estimatedBytes() <= params.max_entry_bytes)` при `max_entry_bytes` = 16 MiB по умолчанию (`ContentAddressedSettings.cpp:88`, дефолт структуры `Parts/PartFolderAccess.h:235`) — 256 ≤ 16 MiB всегда, поэтому ветка `oversized` недостижима, `ProfileEvents::CASPartFolderViewOversizedBypasses` и `LastDecision::OversizedBypass` навсегда нулевые/мёртвые. Настройка и метрика бессмысленны.

Уточнения к формулировке находки: «10 000 retained wide-part views pin gigabytes» верно только с оговоркой, что граница задана `max_entries`, а не байтовым бюджетом (сам бюджет не «отключён», он просто не привязан к реальному размеру). Корректности это не касается: тела манифестов иммутабельны и валидируются по `manifest_id` при каждом обращении (`Parts/PartFolderAccess.cpp:174-190`) — это чистая проблема учёта памяти/наблюдаемости. Память при этом идёт через обычные аллокации, т.е. глобальным трекером видна как давление, а не как тихая порча. Потому P2, а не блокер; оператору доступен обходной путь — снизить `part_folder_cache_max_entries`, но он об этом не узнает, т.к. метрика врёт.

BACKLOG: покрытия нет. Единственные упоминания part-folder кэша: `BACKLOG/ref-protocol.md` {#part-folder-single-flight-manifest-keying} (CAS-019, про single-flight-ключ), `BACKLOG.md` {#part-folder-validate-never-gating} (про `validate=never`) и `BACKLOG/docs-and-cleanup.md:57` (косметика внутренних имён `cas_part_folder_cache_*`) — ни одно не про вес/бюджет. Заявленной в one-liner'е «bytes+count LRU» митигации в коде нет в том смысле, что байтовая половина неоперативна.

Реальный остаток: считать вес по декодированному телу (сумма длин путей + `inline_bytes` + фиксированный overhead на entry) и удалить `Resolved::manifest_size` — его единственный потребитель этот вес, а реестр ref'ов размер тела в принципе не знает; плюс gtest, где манифест с большим inline-телом весит больше пустого (сейчас `gtest_cas_part_folder_view.cpp:50` подаёт `manifest_size=1000` вручную, из-за чего юнит-тесты дефект не видят). Добавлена секция `docs/superpowers/cas/BACKLOG/performance.md` {#part-folder-cache-weight-always-256} (не закоммичено).

## CAS-036 — Из трёх заявленных механик на HEAD жива одна с половиной: тело control-объекта действительно материализуется целиком ДО проверки `object_cap`, а дедупликация ключей JSON остаётся Θ(k²); «read buffer по размеру, объявленному атакующим» и «resize по объявленному размеру zstd-фрейма до проверки» — фактически неверны, а сам «атакующий» по урегулированной модели доверия = держатель bucket-креденшла, т.е. полностью доверенная сторона, и всякий отказ громкий (`MEMORY_LIMIT_EXCEEDED`/`CORRUPTED_DATA`), без тихой порчи. (частично, P3) {#cas-036}

**0. Что вообще появилось после того, как protobuf убрали (контекст one-liner'а).**

One-liner прав в том, что удаление protobuf само по себе не было хардненингом арифметики, но неверно утверждает, что «cap'ов нет»: вместе с текстовыми форматами приехал целый слой лимитов. Коммит `50bbdd1a14f` («cas: formats v3 phase 1 — CasTextFormat: JSON vocabulary, header/trailer lines, zstd arm», подтверждён `git log --follow` по `Formats/CasTextFormat.cpp`) ввёл и построчный cap, и cap на объявленный размер zstd-фрейма; отдельный файл `Formats/CasByteBudget.h` (добавлен в `950d2de9276`) задаёт насыщающую арифметику для pre-PUT бюджетов и прямо объясняет, почему модульная сумма недопустима (`CasByteBudget.h:26-40`). Таблица per-format лимитов: `Formats/CasFormat.cpp:139-159` (`line_cap`/`object_cap`, от 256 B у `cas_blob` до 256 MiB у `cas_part_manifest`/`cas_fold_seal`).

**1. «`readStringUntilEOF`, no cap» — ПОДТВЕРЖДЕНО (единственная живая механика).**

- `Backend/CasObjectStorageBackend.cpp:348-356`: `readObjectRanged(...)`, при `range.whole()` — `readStringUntilEOF(content, *buf); return content;` — без какого-либо лимита.
- `Backend/CasObjectStorageBackend.cpp:587-621`: `ObjectStorageBackend::get` в Native-режиме сначала делает `nativeHead(key)` (`:591`), а затем `gr.bytes = readObjectRanged(*object_storage, key, range, hr->size)` (`:614`). То есть авторитетный размер объекта в этот момент УЖЕ известен (`hr->size`), но не сравнивается ни с чем.
- Cap впервые срабатывает только после материализации: `Formats/CasTextFormat.cpp:384-392` (`openObject`, ветка raw: `stored.size() > t.object_cap` → `CORRUPTED_DATA`) и `:398-403` (ветка zstd). Все call-site'ы идут именно так — сначала `get`, потом `openObject`: `Pool/CasPoolMeta.cpp:100,126,164` (`decodePoolMeta(fresh->bytes)`), `Pool/CasRefProtocol.cpp:896,981,1017,1037,1086`, `Pool/CasManifestReader.cpp:93`, `Tools/CasFsck.cpp:391,673,927`, `Gc/CasOrphanManifestSweep.cpp:259,321`. Отдельно `Formats/CasGcMaintenanceStateFormat.cpp:34-38` проверяет `data.size() > object_cap` — тоже уже по полученным байтам.

Итого фактическая часть верна: между HEAD'ом и cap'ом стоит `String` произвольного размера. Но «OOMs the victim» — завышение: аллокации `std::string` идут через переопределённый `operator new` и учитываются MemoryTracker'ом, поэтому реальный исход — громкое `MEMORY_LIMIT_EXCEEDED` на GC/mount/recovery-потоке вместо запинённого `CORRUPTED_DATA`. Это дефект качества диагностики и устойчивости, а не порча данных. Плюс важное уточнение по достижимости: положить такой объект может только владелец bucket-креденшла, а это по урегулированной позиции и есть вся граница доверия пула (`docs/superpowers/cas/BACKLOG/docs-and-cleanup.md` {#pool-trust-boundary-undocumented}: «the bucket credential IS the whole trust boundary… nothing in the pool protocol authenticates the writer of a control object»); та же сторона может просто удалить пул. Легальный писатель такой объект породить не может: pre-PUT гейты (`Formats/CasFoldSealFormat.cpp:165-192`, `Formats/CasRefCatalogFormat.h:124`, `Formats/CasRefLogFormat.cpp:49-69`) отказывают до записи.

Фикс дешёвый именно потому, что размер известен: передать в `get`/`getStream` ожидаемый cap (или ввести `getControlObject(FormatId, key)`) и отказывать по `hr->size > object_cap` ДО чтения.

**2. «read buffer sized to the attacker's declared size» (`:333-338` в старой нумерации) — ФАКТИЧЕСКИ НЕВЕРНО.**

Речь про `casSizedReadSettings(getReadSettings(), known_size)` (`Backend/CasObjectStorageBackend.cpp:352,396`, определение — `:419-424`: `base.adjustBufferSize(known_size + CAS_FOLD_READ_SLACK_BYTES)`, где `CAS_FOLD_READ_SLACK_BYTES = 4096`, `Backend/CasObjectStorageBackend.h:18`). `ReadSettings::adjustBufferSize` (`src/IO/ReadSettings.cpp:40-49`) берёт `std::min(std::max(1ul, file_size), <настроенный buffer_size>)` — то есть только УМЕНЬШАЕТ буфер и никогда не увеличивает его по размеру объекта. Никакой аллокации «по объявленному размеру» здесь нет.

**3. «declared zstd frame size is allocated before decompression» — СТАЛО НЕВЕРНО (закрыто `50bbdd1a14f`).**

`Formats/CasTextFormat.cpp:398-406`: сначала `ZSTD_getFrameContentSize`, затем отказ при `content == ZSTD_CONTENTSIZE_UNKNOWN/ERROR`, затем `if (t.object_cap != 0 && content > t.object_cap) throw CORRUPTED_DATA`, и только после этого `out.resize(content)` (`:406`) с последующей проверкой `got != content` (`:407-410`). То есть аллокация ограничена `object_cap` формата (максимум 256 MiB, у ref-потоков 64 MiB — `Formats/CasFormat.cpp:144-145`), а не объявленным значением. Порядок «cap → resize» именно тот, которого требует находка.

**4. «one 64 MiB line with a few million distinct keys pins a thread in Θ(k²)» — ПОДТВЕРЖДЕНО как код-шейп, достижимо, но громкость/владелец те же.**

`Formats/CasTextFormat.cpp:173-175`: `if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end()) throw ...; seen_keys.push_back(key);`, где `seen_keys` — `std::vector<String>` (`Formats/CasTextFormat.h:199`), инстанс на одну JSON-строку. Проверка достижимости: `readLine` (`Formats/CasTextFormat.cpp:281-296`) ограничивает строку `line_cap`; у `RefLog`/`RefSnapshot` `line_cap == object_cap == 64 MiB` (`Formats/CasFormat.cpp:144-145`, и это сознательный выбор — обоснование `Formats/CasFormat.cpp:126-135`), а формат Tolerant, так что неизвестные ключи не отбраковываются на первом же, а уходят в `skipUnknown` (`:243-255`) — но `nextKey` УСПЕВАЕТ положить их в `seen_keys`. Значит k ограничен лишь ~64 MiB/5 ≈ 10^7, и квадратичная стоимость реальна. Отличие от находки: это не «GC thread pinned by any control object» (у `Strict`-форматов вроде `cas_ref_ckpt` line_cap = 4 KiB, у `cas_pool_meta` — 64 KiB, `Formats/CasFormat.cpp:141-146`), а именно ref-log/ref-snapshot. Естественная порча (битрот, обрыв записи) миллионы РАЗНЫХ валидных JSON-ключей не производит, так что практический триггер снова — доверенный писатель.

**5. Что в этой находке уже было закрыто раньше и не относится к делу.**

- Bounds-check-before-allocation по length-полям сделан: `Primitives/CasCodecUtil.h:47-62` (`readFixedBytes`: `n > in.available()` → `CORRUPTED_DATA` ДО аллокации), коммит `e9aaec3a309` («CA core: bounds-check before allocation in readFixedBytes»). Единственный потребитель — payload-зона манифеста (`Formats/CasPartManifestFormat.cpp:283-284`).
- Аккумуляции байтов, которую можно было бы переполнить в бюджете ref-транзакции, нет по построению: `Formats/CasRefLogFormat.cpp:45-69` («checked via `encodedOpSize`, one op at a time — no accumulation»).

**Поиск по BACKLOG.** Существующего anchor'а на этот класс нет: `grep -ni "unbounded|object_cap|OOM|quadratic"` по `docs/superpowers/cas/BACKLOG.md` и `BACKLOG/*.md` даёт только несвязанное (`BACKLOG/ref-protocol.md:59` {#recovery-repair-buffer-unbounded} — про буферизацию восстановительного replay, другой путь; `BACKLOG/operability-and-introspection.md:23` [B165] — про RSS в soak; `BACKLOG/performance.md` {#part-file-suffix-allowlist-memory} — про write-path inline-буферизацию). Ближайшая смысловая связка — {#pool-trust-boundary-undocumented} (CAS-027), которая и объясняет, почему приоритет низкий.

**Что реально осталось (заведено).** Новый раздел `docs/superpowers/cas/BACKLOG/formats-and-storage.md` {#control-object-read-precap-materialization} (не закоммичен): (а) прогать cap по `hr->size` до чтения тела, чтобы ошибка была запинённой `CORRUPTED_DATA`, а не `MEMORY_LIMIT_EXCEEDED`; (б) заменить линейный `seen_keys` на сортированное/хешированное множество либо ввести лимит на число ключей в объекте. Оба — P3: громкий отказ, доверенный триггер, ноль тихой порчи.

## CAS-037 — Обёртка `readIntText` без проверки переполнения — реальный код-шейп, и два настоящих остатка есть (`std::stoull` принимает `-1` в трёх разборах GC-ключей, из которых один даёт `max_gen + 1 == 0`; `location.offset + location.length` может завернуться и схлопнуть окно чтения в EOF), но центральный тезис «wrapping defeats every decoder range gate» логически несостоятелен, а `-1` в телах, underflow `blob_header_len - 1`, аллокация по length-полю и «Poco multiplication» — неверны либо недостижимы. (частично, P3) {#cas-037}

**1. «Every CAS numeric field silently wraps mod 2^64» — код-шейп ПОДТВЕРЖДЁН.**

`Formats/CasTextFormat.cpp:202-232`: `readU64String` (через `readIntText` по `ReadBufferFromMemory` с обязательным `buf.eof()` и непустой строкой), `readU64Number` (`:216-224`, чистый `readIntText(v, in)`), `readU32Number` (`:226-232`, единственный, у кого есть диапазонный гейт: `v > numeric_limits<uint32_t>::max()` → `CORRUPTED_DATA`). Дефолт шаблона действительно без проверки: `src/IO/readIntText.h:246-250` — `template <ReadIntTextCheckOverflow check_overflow = ReadIntTextCheckOverflow::DO_NOT_CHECK_OVERFLOW>`. Так что «21-цифровое число заворачивается» — правда.

**2. Но вывод «wrapping defeats every decoder range gate» — НЕСОСТОЯТЕЛЕН.**

Заворачивание было бы примитивом обхода, если бы существовало «истинное» значение, которое гейт отсекает, а завёрнутое — пропускает. Здесь такого нет: тот, кто пишет объект, свободно пишет сразу любое значение В диапазоне, поэтому wrap не даёт атакующему НИЧЕГО, чего не даёт обычная запись. Wrap имеет значение ровно в одном смысле — неканоническое написание принимается там, где строгий парсер отказал бы (`readIntText` также принимает ведущий `+`: `src/IO/readIntText.h:51-73`). Это гигиена канонической формы, не безопасность.

Кроме того диапазонные гейты реально стоят ПОСЛЕ парсинга и ловят любое значение, включая завёрнутое:
- `Formats/CasPoolMetaFormat.cpp:38-48` `validatePoolBlobHeaderLen`: `>= 240`, кратно 8, `<= 16384`; вызывается на декоде — `:171`; плюс `gcs == 0` → `CORRUPTED_DATA` (`:164-165`), `min_reader_generation` против `G_BUILD` (`:174-178`).
- `Formats/CasPartManifestFormat.cpp:239-245`: у Blob-записи обязательны `ha/h/sz`, ширина hex-дайджеста сверяется с алгоритмом (`:241-245`), порядок путей строго возрастающий (`:262-267`), в конце — пересчёт `payload_digest` по полностью декодированному телу (`:297-301`).

**3. «`-1` принимается» — по телам НЕВЕРНО, по КЛЮЧАМ ВЕРНО (настоящий остаток).**

В телах: `readIntText` для беззнакового типа явно отказывает на `'-'` — `src/IO/readIntText.h:82-92` («Unsigned type must not contain '-' symbol», `CANNOT_PARSE_NUMBER`), а `JsonObjectReader::guarded` (`Formats/CasTextFormat.cpp:128-139`) переводит `CANNOT_PARSE_NUMBER` в `CORRUPTED_DATA`. То есть «bodies accept negative» — фактически неверно.

В разборе ключей из LIST — верно. `Gc/CasGc.cpp:1488`, `:1619`, `:4094` используют `std::stoull` в `try { } catch (...) { return; }`. Catch снимает мусор (`invalid_argument`/`out_of_range`), но `std::stoull("-1")` НЕ бросает: проверено через `.claude/tools/cppexpr.sh --plain` — `std::stoull("-1") -> 18446744073709551615`, `std::stoull("  -0000001") -> 18446744073709551615`. Последствия по сайтам:
- `:1488` — `generation` попадает в `listed_generations`, `listed_max_generation` становится `UINT64_MAX`; шаг вниз идёт по членам множества, так что цена — лишний LIST по несуществующей генерации. Мелочь.
- `:1619` — `attempt` затем ДОКАЗЫВАЕТСЯ обратной сборкой ключа: `if (layout.foldSealKey(generation, attempt) != k.key) return;` (`:1625`). Здесь фальшивое значение отсеивается по построению — сайт защищён.
- `:4094` — это и есть настоящий остаток: `max_gen = std::max(max_gen, stoull(...))` → `UINT64_MAX`, а на `:4102` `const uint64_t generation = max_gen + 1` заворачивается в 0, то есть путь REBUILD получает номер генерации, который комментарий прямо над циклом (`:4081`: «putDeterministicArtifact must never collide with debris of the lost era») запрещает. Отказ при этом остаётся fail-closed (write-once публикация детерминированного артефакта по уже занятому ключу упрётся в precondition), но инвариант нумерации нарушен.

Важно: тот же урок в этом дереве УЖЕ выучен и зафиксирован в коде — `ContentAddressedMetadataStorage.cpp:332-340` использует `std::from_chars` именно потому, что «`std::stoull` … silently negates modulo 2^64» (коммит `fc89b827d74`, «cas: §3 part_folder_validate — fix age-parse negative-wraparound»). Три сайта GC этой дисциплины не получили — это несогласованность внутри дерева, а не новый класс.

**4. «planted manifest `sz` produces a wrapped read window» — ЧАСТИЧНО ВЕРНО, но исход громкий.**

Путь: `Formats/CasPartManifestFormat.cpp:222` (`sz = r.readU64Number()`) → `e.blob_size = *sz` (`:247`) → `Pool/CasManifestReader.cpp:155-159` (`BlobLocation{.offset = meta.blob_header_len, .length = entry.blob_size}`) → `ContentAddressedMetadataStorage.cpp:2004-2006`: `StoredObject(physicalKey(location.key), path, location.offset + location.length)` и `ReadBufferFromFileView(std::move(impl), path, location.offset, location.offset + location.length)`. Сумма считается дважды и модульная; `offset` валиден (`>= 240`, см. п.2), значит для заворота нужен `sz` вблизи `UINT64_MAX`.

Что происходит при завороте: `right_bound < left_bound`. Валидации `left <= right` в конструкторе нет (`src/IO/ReadBufferFromFileView.cpp:13-31` — только `impl->seek(left_bound)` и `resizeWorkingBuffer()`), а `resizeWorkingBuffer` (`src/IO/ReadBufferFromFileView.cpp`) при `file_offset_of_buffer_end > getRightBound()` урезает рабочий буфер до `max(size - extra, 0) == 0` и выставляет `file_offset_of_buffer_end = right_bound`; следующий `nextImpl` видит `current_position == getRightBound()` и возвращает `false`. То есть окно схлопывается в мгновенный EOF: пустое чтение, дальше громкая ошибка на уровне MergeTree, без выхода за границы буфера и без подмены данных. Побочно `tryGetFileSize() = right_bound - left_bound` (`src/IO/ReadBufferFromFileView.h:22`) заворачивается в огромное значение — рассогласование отчётного размера.

Отдельно: НЕ-заворачивающийся огромный `sz` безопасен — второй ranged-путь клампится явно: `Backend/CasObjectStorageBackend.cpp:373-382` (`available = object_size - range.offset`, `to_read = min(*range.length, available)`, `content.resize(got)`). Заворот `range.offset + *range.length` в `setReadUntilPosition` (`:378`, `:414`) там же безопасен: вызов идёт ДО чтения (`file_offset_of_buffer_end == 0`), так что бросок `ReadBufferFromRemoteFSGather::setReadUntilPosition` («Attempt to set read until position before already read data») недостижим, а фактическое число байт задаётся клампом, не подсказкой.

Правка принадлежит стороне CAS (валидировать `sz` при декоде манифеста и/или считать сумму насыщающе через `addByteBudget`, `Formats/CasByteBudget.h:27-30`), а не generic-буферу — по политике минимизации связности с upstream.

**5. Остальные пункты находки — неверны либо недостижимы.**

- «`blob_header_len - 1` — тот же класс неконтролируемой арифметики»: недостижимо. `blob_header_len` валидируется на декоде пула (`Formats/CasPoolMetaFormat.cpp:38-48`, вызов `:171`) и при создании пула тем же предикатом с `BAD_ARGUMENTS`; минимум 240, кратность 8, максимум 16384 — underflow `-1`/`-2` невозможен, обоснование минимума расписано в `Formats/CasPoolMetaFormat.cpp:20-37`.
- «`published_at_ms` overflows the Poco timestamp multiplication»: на HEAD там ДЕЛЕНИЕ — `ContentAddressedMetadataStorage.cpp:1655`: `Poco::Timestamp::fromEpochTime(static_cast<time_t>(resolved->published_at_ms / 1000))`, с явной ветвью `published_at_ms == 0 → Poco::Timestamp(0)` (`:1652-1653`). Максимальный эффект от подложенного `ts` — бессмысленное значение `getLastModified` (косметика); умножения, о котором говорит находка, в коде нет.
- «fsck referenced-bytes accumulator»: `Tools/CasFsck.cpp:695` `report.referenced_logical_bytes += e.blob_size;` — счётчик отчёта, ни одно решение об удалении на нём не строится; максимум искажённое число в отчёте.
- «аллокация по length-полю»: закрыто. `Primitives/CasCodecUtil.h:47-62` — `readFixedBytes` отказывает `CORRUPTED_DATA` при `n > in.available()` ДО аллокации, с комментарием ровно про «под memory tracker'ом это вылезет как MEMORY_LIMIT_EXCEEDED вместо запинённого CORRUPTED_DATA»; коммит `e9aaec3a309`.
- «bodies accept non-canonical numeric spellings the key parser rejects»: полу-верно и с обратным знаком. Ведущий `+` и заворот в телах принимаются (`src/IO/readIntText.h:51-73`, `:246`), а `-` — нет (п.3); при этом именно КЛЮЧЕВОЙ парсер (`std::stoull`) как раз более снисходителен (принимает `-`, пробелы, знак). То есть направление «тела снисходительнее ключей» неверно.
- Накопительных бюджетов, где модульная сумма ответила бы «влезает», нет: `Formats/CasRefLogFormat.cpp:45-69` («no accumulation»), а pre-PUT предикаты используют насыщающую арифметику (`Formats/CasByteBudget.h:27-40`, применение — `Formats/CasFoldSealFormat.cpp:165-192`, `Formats/CasRefCatalogFormat.cpp`).

**Поиск по BACKLOG.** Anchor'а по этому классу нет: `grep -ni "stoull|from_chars|readIntText|integer"` по `docs/superpowers/cas/BACKLOG.md` и `BACKLOG/*.md` не даёт ничего релевантного. Смежный уже отслеживаемый пункт того же класса u64-охраны — `docs/superpowers/cas/BACKLOG.md:411` {#reftxnid-wraparound-guard-missing} («`nextRefTxnId` lacks a `UINT64_MAX` wraparound guard»); он про другой счётчик, поэтому не покрывает эту находку.

**Что реально осталось (заведено).** Новый раздел `docs/superpowers/cas/BACKLOG/formats-and-storage.md` {#numeric-parse-and-window-wrap} (не закоммичен): (а) три `std::stoull` → `std::from_chars` + защита `max_gen + 1` от заворота (`Gc/CasGc.cpp:1488`, `:1619`, `:4094`, `:4102`); (б) валидировать `sz` манифеста / считать окно насыщающе (`ContentAddressedMetadataStorage.cpp:2004-2006`). P3: триггер — только доверенный держатель bucket-креденшла ({#pool-trust-boundary-undocumented}), исход в обоих случаях громкий (fail-closed precondition либо пустое чтение с последующей ошибкой), тихой порчи данных нет.

## CAS-046 — Все части описания кода верны на HEAD (локальный scratch = полные байты части, без резервирования/учёта/квоты/лимита, удаляется только в конце транзакции, стартовой уборки нет), но это уже дважды затреканный DESIRABLE-класс, а не новый дефект: отказы громкие (ENOSPC fail-loud), тихой порчи нет. (подтверждено, P2) {#cas-046}

Пути в находке устаревшие (`CA/ContentAddressedTransaction.cpp` — снапшот до 592b9b83568); актуальный файл — `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`, номера строк в анкорах не совпадают.

Что подтверждается на HEAD:

1. Полный спилл в локальный scratch. `ContentAddressedTransaction.cpp:906-917` открывает `Cas::CaContentWriteBuffer(metadata_storage.scratchPath(), ...)`; конструктор (`ContentAddressedTransaction.cpp:1788-1814`) создаёт `temp_dir + "/" + getRandomASCIIString(32) + ".tmp"` и пишет ВСЁ содержимое файла через `WriteBufferFromFile sink` (хеш считается стримом поверх sink — `hashing = Cas::makeBlobHashingWriteBuffer(hash_algo, *sink)`), т.е. байты блоба ложатся на локальный том целиком. Это по-прежнему дефолт: `ContentAddressedSettings.cpp:92` — `staging_backend` = `"local"`, `s3` только opt-in.

2. Дефолтный scratch — на серверном data-томе: `MetadataStorageFactory.cpp:236` — `default_scratch_path = <server path>/disks/<name>/cas_scratch/`, и `MetadataStorageFactory.cpp:238` его `create_directories`. Анкор находки (`:233-238`) здесь совпал.

3. Диск не отдаёт свободного места и резервирование фиктивно: `src/Disks/DiskObjectStorage/DiskObjectStorage.h:68-70` — `getTotalSpace/getAvailableSpace/getUnreservedSpace` возвращают `{}`; `DiskObjectStorage.cpp:561+` `tryReserve` при `available_space == nullopt` резервирует безусловно. ВАЖНО: это свойство ВСЕХ object-storage-дисков апстрима, а не CAS; специфика CAS в том, что она единственная кладёт полный объём части ещё и на локальный том, который никто не резервировал.

4. Нет ни квоты, ни `statvfs`-проверки, ни метрики. По всему каталогу CAS нет ни одного обращения к свободному месту (grep по `statvfs|space|quota` в `.../ContentAddressed/` даёт только совпадения в комментариях/форматах), и в `src/Common/CurrentMetrics.cpp`/`ProfileEvents.cpp` нет ни одного `*Scratch*` счётчика — «нет видимости» верно.

5. Файлы живут до конца транзакции, а не до конца загрузки блоба. `uploadPendingBlobs` (`ContentAddressedTransaction.cpp:256-309`) только читает `pb.staging_key` в `source.open`, ничего не удаляя; удаление — `cleanupPendingTempFiles` (`ContentAddressedTransaction.cpp:165-175`, `fs::remove` для `StagingBackend::Local`), вызываемая в конце `commit` (`:452`, `:515`, после `committed = true`) и как backstop в деструкторе (`:112`). Значит пик = сумма байт всех in-flight частей транзакции.

6. Стартовой/маунт-уборки локального scratch нет. Для S3-стейджинга сweeper есть и он назван прямо в комментарии (`ContentAddressedTransaction.cpp:189` — `Cas::sweepOwnMountStaging`), для локального scratch аналога нет: `scratchPath()` (`ContentAddressedMetadataStorage.h:385`) используется только на путях записи, `cas_scratch` встречается лишь в фабрике/настройках/тестах (grep по всему `src/`). Т.е. после нечистого рестарта `*.tmp` остаются навсегда — утверждение верно. Уточнение к формулировке находки: обычный exception/cancel НЕ течёт — `CaContentWriteBuffer::~CaContentWriteBuffer`/`cancelImpl` (`:1850-1859`, `:1896-1908`) удаляют temp, пока `temp_ownership_transferred == false`; течёт именно kill -9 / падение процесса.

7. Единственный настоящий остаточный дефект кода — inline-overflow. `ContentAddressedTransaction.cpp:958-975`: файл пишется блоком
   `{ WriteBufferFromFile tmp(temp_path); tmp.write(...); tmp.finalize(); }`
   и только ПОСЛЕ этого блока ставится `SCOPE_EXIT({ if (!staged) ... remove(temp_path) })` (`:973`). Если `write`/`finalize` бросит (ENOSPC), частично записанный файл останется — деструктор `WriteBufferFromFile` его не удаляет, а в `pending_blobs` он ещё не зарегистрирован, так что `cleanupPendingTempFiles` его тоже не увидит. Reachability: ветка — заявленный safety-net для inline-кандидата размером > `INLINE_CAP`, плюс требуется отказ локальной записи; отказ громкий (исключение наверх), тихой порчи нет. Это P3-хвост того же класса, что и «orphan sweeper», который его и подобрал бы.

Покрытие в BACKLOG (всё уже затрекано, находка не нова):
- `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:41-43` в секции `{#disk-error-audit-followups-2026-07-21}`: «DESIRABLE: free-space guard + orphan sweeper for `scratch_path` — no `statvfs` check before a local staging write, and orphaned `*.tmp` files from an unclean restart are never swept (the S3 staging prefix has a sweeper; local scratch does not)» — покрывает пункты 4, 6 и 7 дословно.
- `docs/superpowers/cas/BACKLOG/performance.md:129` в секции `{#scale-findings}`: `[scratch=full-part]` — «100 GiB merge → 93 GiB scratch; a part larger than local free scratch cannot be written… largely addressed by the (opt-in) S3-native staging» — покрывает пункты 1, 3, 5. Рядом `:130` `[replicated double-spill]` (186 GiB на реплике) и `:128` `[idle-scratch-debris]` — тот же класс.
- Вердикт аудита дисковых ошибок в шапке той же секции (`operability-and-introspection.md:30-31`) прямо фиксирует: «staging ENOSPC fail-loud» — то есть класс отказа громкий, а не тихая порча, поэтому это P2-трек, а не релизный блокер.

Новых записей в BACKLOG не добавлял: остаток (inline-overflow leak) целиком укладывается в уже существующий пункт про orphan sweeper для `scratch_path`, отдельная запись была бы дублем.

Итого: описание кода верно, «impact» верен как масштабная/операционная характеристика, но новизны нет и релизного блокера нет; P2 (после релиза): (а) `statvfs`-guard перед локальной записью стейджинга, (б) sweeper осиротевших `*.tmp` при монтировании, (в) метрика scratch-байт, (г) перенос `SCOPE_EXIT` до открытия файла в inline-overflow ветке.

## CAS-047 — Форма кода описана верно (один процессный пул, 16 потоков, queue_size == max_threads, enqueue блокирующий), но привязанное следствие ложно: блокировка на enqueue — это штатный backpressure, а не сериализация и не дедлок; размер — серверная настройка, тюнинг уже затрекан. (by-design, P3) {#cas-047}

Пути устарели (`CA/Pool/...` — снапшот до 592b9b83568); актуальные — `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.cpp` и `.../ContentAddressedTransaction.cpp`. Номера строк в анкорах не совпадают.

Верно на HEAD:

1. Пул процессный (один экземпляр на процесс): `Pool/CasBlobUploadPool.cpp:31-33` — статические `std::mutex pool_mutex; std::unique_ptr<ThreadPool> pool_instance;`, `initializeBlobUploadPool` (`:36-50`) бросает `LOGICAL_ERROR` при повторной инициализации, `blobUploadPool()` (`:52-59`) отдаёт этот единственный пул. Инициализация одна на процесс: `programs/server/Server.cpp:1722`, `programs/local/LocalServer.cpp:428`, `programs/disks/DisksApp.cpp:559`. То есть да, все CAS-диски процесса делят пул — и это ЗАЯВЛЕННЫЙ дизайн, а не побочный эффект: заголовок `Pool/CasBlobUploadPool.h:13-25` описывает его как «Server-wide pool for the parallel intra-part blob upload fan-out», намеренно отделённый от `IObjectStorage::getThreadPoolWriter` именно чтобы вложенная отправка (S3 multipart) не могла дать wait-on-self дедлок.

2. Размер 16 и queue_size == 16: `src/Core/ServerSettings.cpp:151` — `cas_blob_upload_pool_size` = 16 (документировано в `docs/en/antalya/cas/configuration.md:125`). Конструктор используется 4-аргументный (`CasBlobUploadPool.cpp:43-48`), а он делегирует в `ThreadPool.h`-реализацию с `max_free_threads_ = queue_size_ = max_threads_` (`src/Common/ThreadPool.cpp:187-195`), и `queue_size` затем `= max(queue_size_, max_threads)` (`ThreadPool.cpp:206`). Итог: очередь = 16 = число потоков. Утверждение находки точное.

3. Enqueue блокирующий и без дедлайна: fan-out планирует через `runner.enqueueAndGiveOwnership(...)` без `wait_microseconds` (`ContentAddressedTransaction.cpp:1767`), а `ThreadPoolCallbackRunnerLocal::enqueueAndGiveOwnership` в этом случае зовёт `pool.scheduleOrThrowOnError` (`src/Common/threadPoolCallbackRunner.h:229-231`), т.е. `scheduleImpl<void>(..., std::nullopt)` (`ThreadPool.cpp:511`), где при полной очереди выполняется `job_finished.wait(lock, pred)` без таймаута (`ThreadPool.cpp:332-341`). «Undeadlined» — верно буквально.

Что НЕ верно / что нельзя приписывать этой форме:

4. «Все загрузки сериализуются» — нет. 16 задач выполняются ПАРАЛЛЕЛЬНО; полная очередь означает лишь, что отправитель ждёт освобождения слота, т.е. классический backpressure с сохранением полной загрузки пула. Сериализации до одного потока не возникает нигде.

5. Дедлока/wedge нет, и это проверяемо, а не на веру. Вызов один-единственный: `Cas::fanOutBlobUploads(*st.build, requests, Cas::blobUploadPool())` из `ContentAddressedTransaction::uploadPendingBlobs` (`ContentAddressedTransaction.cpp:309`) — grep по `blobUploadPool` во всём `src/` даёт только его и тесты. Отправляющий поток слот пула не занимает (комментарий-инвариант `ContentAddressedTransaction.cpp:1728-1735` и `CasBlobUploadPool.h:18-22`), задачи выполняют `txn->uploadBlobDetached(req_by_value)` (`ContentAddressedTransaction.cpp:1770`), а `PartWriteTxn::uploadBlobDetached` (`Pool/CasPartWriteTxn.cpp:177+`) на этот пул ничего не планирует (её вложенный параллелизм — писательский пул S3, другой пул). Поэтому пока в пуле есть хоть одна работающая задача, слоты освобождаются, и ожидание отправителя конечно. Более того, каждая попытка загрузки ограничена дедлайном операции `CasRequestController` (90 с, ≤16 попыток — `docs/en/antalya/cas/operations/troubleshooting.md:20`), т.е. ожидание ограничено сверху, а не бесконечно.

6. «2C + 1 задач; часть на 100 колонок ставит 201» — арифметика придумана. Fan-out группирует по УНИКАЛЬНОМУ `BlobRef`: одна задача на уникальный блоб части (`ContentAddressedTransaction.cpp:1691-1712` — `std::map<BlobRef, BlobUploadRequest> grouped`, дедуп hardlink-копий), плюс inline-записи вообще не попадают в `pending_blobs` (`:263-265`). Никакой формулы «2C+1» в коде нет; реальная величина по замерам — ~8 объектов на колонку (≈239 PUT/part на широком профиле, `docs/superpowers/cas/BACKLOG/performance.md:36-38`), то есть направление верное, число — нет.

7. Триггер «более 16 in-flight загрузок» описывает не сбой, а нормальный режим работы, для которого пул и введён: `[write-path stage 1] parallel intra-part blob upload — LANDED (2026-07-24)` (`docs/superpowers/cas/BACKLOG/performance.md:24`) — именно этот fan-out дал 58.41s → 30.26s (CA-vs-plain 3.0x → 1.59x).

Покрытие в BACKLOG: `docs/superpowers/cas/BACKLOG/performance.md:47-49`, секция `{#writepath-candidates-post-stage1}`, пункт (2) «S3 client concurrency/connection tuning for the upload pool — with 16-33 threads now issuing PUTs concurrently, client-side limits (connections, per-request concurrency) may cap overlap. Status: MEASURE» — ровно тюнинг степени параллелизма этого пула. Кроме того `docs/en/antalya/cas/operations/troubleshooting.md:20` уже рекомендует ПОНИЖАТЬ `cas_blob_upload_pool_size` при устойчивых `SlowDown`, т.е. настройка — штатный рычаг оператора в обе стороны.

Реальный остаток (P3, не блокер и не тихая порча): ожидание в `scheduleImpl` не проверяет отмену запроса, поэтому INSERT/merge, застрявший на заполненной очереди, не реагирует на `KILL QUERY` до освобождения слота (ограничено сверху 90-секундным дедлайном операции на задачу). Хотел бы отметить, что это свойство общего `ThreadPool` апстрима, а не CAS-кода: лечится либо `scheduleOrThrow` с дедлайном, либо `queue_size > max_threads`, и то и другое — микро-тюнинг внутри уже затреканного пункта (2). Новую запись в BACKLOG не добавлял: отдельный пункт был бы дублем `{#writepath-candidates-post-stage1}` (2), а изменение формы очереди — это как раз тот замер, который там и запланирован.

## CAS-048 — Форма подтверждена — публикация пустой покрывающей части действительно идёт под `DataPartsLock`, но путь редкий (только DROP/REPLACE PARTITION), работа маленькая и ограниченная, отказ громкий, а на обычном object-storage-диске тот же lock уже удерживается на время записи части. (частично, P3) {#cas-048}

**Что подтверждается на HEAD (3ff0301f261).**

Якорь смещён (снапшот старый), но код на месте: `src/Storages/MergeTree/MergeTreeData.cpp:5937-5939`

```
        if (new_data_part->getDataPartStorage().isContentAddressed()
            && new_data_part->getDataPartStorage().hasActiveTransaction())
            new_data_part->getDataPartStorage().commitTransaction();
```

Это внутри `MergeTreeData::removePartsInRangeFromWorkingSetAndGetPartsToRemoveFromZooKeeper` (`MergeTreeData.cpp:5842-5843`), которая принимает `DataPartsLock & lock` от вызывающего, т.е. блокировка удерживается вызывающим на всём протяжении. Все реальные call sites берут её эксклюзивно непосредственно перед вызовом: `src/Storages/StorageReplicatedMergeTree.cpp:2982` (`auto data_parts_lock = lockParts();`) → `:2987` (`executeDropRange`, `create_empty_part=true`), а также `:9343` и `:9608` (REPLACE/MOVE PARTITION, там `create_empty_part` по умолчанию `true`, `src/Storages/MergeTree/MergeTreeData.h:921`), и `MergeTreeData.cpp:5766` для plain-MergeTree `removePartsInRangeFromWorkingSet`.

Под этой же блокировкой лежит и создание части целиком: `createEmptyPart` (`MergeTreeData.cpp:5913-5914`) и `renameTempPartAndAdd(..., /*rename_in_transaction=*/false)` (`:5916`).

Объём удалённого I/O под блокировкой на CA-диске подтверждается: `ContentAddressedTransaction::commit` (`src/Disks/.../ContentAddressed/ContentAddressedTransaction.cpp:434`) выполняет всю публикацию — `stageManifest` + `precommitAdd` + `uploadPendingBlobs` + `promoteBuild` (`ContentAddressedTransaction.cpp:409-425`), т.е. PUT манифеста-precommit, загрузку блобов (`fanOutBlobUploads`, `:309`) и CAS-аппенд ref-лога с ретраями при конкуренции. Комментарий в `commit` прямо фиксирует: «All pending blobs have been uploaded in publishStaging» (`:514`). То есть на CA-диске сеть сконцентрирована именно в `commitTransaction`, а не в записи файлов (tmp→final rename — «pure overlay re-key», см. комментарий `ContentAddressedTransaction.cpp:453`).

Итог по механике: утверждение аудита «CA part publish выполняется под `DataPartsLock`» — верно.

**Что в утверждениях аудита неточно / преувеличено.**

1. «prev CAS-006 закрыл это» — некорректная ссылка. Off-lock-публикация действительно появилась, но в другом месте: `MergeTreeData::Transaction::renameParts` (`MergeTreeData.cpp:8995-9021`), коммит `77484196b0d` («cas: close part disk-storage transactions in renameParts…», 2026-07-17), чьё сообщение прямо говорит «Also moves the object-storage disk commit off the data_parts lock». Комментарий на `MergeTreeData.cpp:9006-9012` фиксирует контракт. Этот коммит НЕ трогал путь покрывающей части — она добавлена раньше, `6a0e506533c` («CAS M6 B61(b): persist DROP_RANGE empty covering part on content-addressed disk», 2026-06-04). Так что «было исправлено, но не применено к этому пути» — фактически верно, только «prev CAS-006» этого раунда (`docs/superpowers/cas/2031-triage.md:24`, RENAME DATABASE) — другое issue; настоящий закрывающий коммит — `77484196b0d`.

2. «Impact: блокирует каждый SELECT, планирование мержей и мутацию part-set на время удалённой публикации» — форма верна (`DataPartsLock` — эксклюзив над `data_parts_mutex`, `MergeTreeData.cpp:544`), но масштаб преувеличен:
   - Публикуется ПУСТАЯ покрывающая часть: несколько мелких блобов + манифест + один аппенд ref-лога. Это не «remote publish» части данных.
   - На обычном object-storage-диске тот же самый `DataPartsLock` уже удерживается на время `createEmptyPart`, где запись файлов части идёт прямо в объектное хранилище. То есть «object-store I/O под parts lock на этом пути» — не CA-специфика, а унаследованное upstream-поведение; CA лишь переносит момент I/O из записи в commit и добавляет CAS ref-лога.
   - Сценарий деградации — «throttling или 5xx бакет»; в этом состоянии таблица на CA/S3 нездорова целиком (чтения, мержи, инсерты тоже идут в тот же бакет), поэтому «stall иначе здоровой таблицы» не получается.
   - Триггер — только DDL `DROP PARTITION` / `DROP PART` / `REPLACE PARTITION` (и репликационная запись `DROP_RANGE`), не пользовательский поток запросов.

3. Класс отказа — громкий fail-closed: исключение из `commitTransaction` уходит наверх из `executeDropRange`, запись очереди ретраится; молчаливого повреждения нет. Ничего не публикуется частично незаметно (rollback-компенсация в `commit`, `ContentAddressedTransaction.cpp:496-501`).

**Почему не «просто починить».** Комментарий `MergeTreeData.cpp:5921-5936` объясняет, почему `commitTransaction` стоит именно здесь: путь по построению откатывает in-memory-транзакцию (`transaction.rollback(&lock)`, `:5942`), чтобы часть осталась `Outdated`, и потому НИКОГДА не доходит до `Transaction::commit` — единственного другого места, где закрывается disk-транзакция. Вынести публикацию за пределы блокировки нельзя локально: `empty_info` вычисляется из `parts_to_remove`, добытых под этой же блокировкой (`:5897-5905`), и `renameTempPartAndAdd` тоже требует её. Реальная починка — трёхфазная перестройка вызывающего (посчитать под блокировкой → отпустить → создать+опубликовать → взять снова), что рискованнее самой проблемы.

**BACKLOG / история.** Прямого покрытия нет: `grep -niE "DataPartsLock|under the lock|covering part|DROP PARTITION"` по `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` даёт только несвязанную строку `operability-and-introspection.md:100`. В `docs/superpowers/cas/2031-triage.md` этого сюжета тоже нет. Как непокрытый остаточный пункт добавлен (uncommitted) раздел `docs/superpowers/cas/BACKLOG/performance.md` → `{#covering-part-publish-under-datapartslock}`.

**Что реально осталось.** Один узкий пункт удержания блокировки на редком DDL-пути: время удержания `DataPartsLock` на DROP/REPLACE PARTITION включает полную CA-публикацию пустой покрывающей части. Это не гейт релиза — P3, трекается в BACKLOG.

## CAS-049 — Сериализация подтверждена и в большинстве мест сознательна; реальный остаток — отсутствие кооперативной отмены (GC-раунд и FSCK нельзя прервать, SQL FSCK не убивается KILL QUERY и не передаёт deadline), при этом «shutdown ждёт FSCK» неверно, а «unbounded scans» неточно — раунд ограничен work-бюджетами. (частично, P2) {#cas-049}

**Якоря на HEAD (3ff0301f261).** Пути переехали (`592b9b83568`), номера строк из снапшота устарели:
- `CA/Gc/CasGcScheduler.cpp:213`, `:245` → фактически `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp:245` (`std::lock_guard round_lock(gc_round_mutex);` в `runOneRoundNow`) и `:298` (то же в `loop`). На `:213` никакой блокировки нет — это тело `runRoundLogged`.
- `:67-79` (`stop()` + `join()`) → `Gc/CasGcScheduler.cpp:72-88`.
- `CA/ContentAddressedMetadataStorage.cpp:739-745` (`lifecycle_mutex` через весь `runFsck`) → `ContentAddressedMetadataStorage.cpp:1051-1063`.
- «тот же mutex на `:663`, `:691`, `:711`» → фактически `:933` (`forgetDisk`), `:982` (`gcStop`), `:1012` (`gcStart`), `:1055` (`runFsckNow`). Объявление и порядок взятия: `ContentAddressedMetadataStorage.h:649-650` (`lifecycle_mutex` → `gc_scheduler_mutex` → `pointer_mutex`).

**Что подтверждается.**

1. Раунд GC не имеет отмены. `stop()` (`Gc/CasGcScheduler.cpp:72`) выставляет `stopping`, будит `wake` и делает `thread.join()` (`:79`) / `hb_thread.join()` (`:81`). Ни `Gc/CasGc.h`, ни `Tools/CasFsck.cpp`, ни `Gc/CasGc.cpp` не содержат ни одного cancel-хука (`grep -rn "cancel\|stop_token\|should_stop\|deadline" Gc/CasGc.h` — пусто). Значит, `join()` ждёт завершения текущего раунда целиком. Комментарий в `loop` (`:295-297`) сам это фиксирует как «accepted extra round».
2. Лестница сериализации существует. `gcStop` (`:978-1003`) берёт `lifecycle_mutex` + `gc_scheduler_mutex`, затем `snapshot->stop()`; `forgetDisk` (`:926-969`) — то же плюс `pool->forgetDisk([...]{ scheduler->stop(); })`; `gcStart` (`:1004-1046`) — те же две блокировки. Синхронный раунд держит `gc_scheduler_mutex` целиком (`:389`, `:619`, `:665`, комментарии `:381` и `:611`: «Hold gc_scheduler_mutex for the whole round»). Значит `GC STOP`/`FORGET`/`GC START` действительно ждут и синхронный раунд (через `gc_scheduler_mutex`), и фоновый (через `join()` внутри `stop()`).
3. `runFsckNow` держит `lifecycle_mutex` на весь скан (`:1051-1063`), поэтому `FORGET`/`GC STOP`/`GC START` ждут FSCK. Это ЗАЯВЛЕНО как намеренное: комментарий `:1052-1053` — «Held for the WHOLE scan, so a concurrent lifecycle-control verb (FORGET / GC STOP / GC START) cannot race the disk out from under an in-flight FSCK» (тот же тезис в `ContentAddressedMetadataStorage.h:212`).
4. SQL-путь FSCK действительно не передаёт `on_progress`/`deadline`/`partial_on_deadline`: `ContentAddressedMetadataStorage.cpp:1063` — `return Cas::runFsck(*store(), detail);`, при том что сигнатура их принимает (`Tools/CasFsck.h:269-271`), а CLI их передаёт: `programs/disks/CommandFsck.cpp:67` — `Cas::runFsck(*ca->store(), detail, on_progress, deadline, partial, namespace_prefix)`. Плюс SQL-вход зовёт `runFsckNow(/* detail= */ false)` из `src/Interpreters/InterpreterSystemQuery.cpp:2599`, и скан не проверяет отмену запроса — значит `SYSTEM CAS FSCK` не прерывается ни `KILL QUERY`, ни `max_execution_time`.

**Что неверно или неточно.**

1. «shutdown серилизуется за in-flight FSCK» — НЕВЕРНО. `ContentAddressedMetadataStorage::shutdown` (`:887-908`) берёт только `gc_scheduler_mutex` (`:891`) и `pointer_mutex`; `lifecycle_mutex` он НЕ берёт, а `runFsckNow` не берёт `gc_scheduler_mutex`. Пул удерживается живым через `shared_ptr` (`store()`), так что shutdown при работающем FSCK проходит. Единственная задержка shutdown — ожидание раунда GC: `gc_scheduler_mutex` для синхронного и `old_scheduler->stop()` для фонового, и это прямо задокументированный выбор приоритета: «Wait for any in-flight synchronous round to finish cleanly first … unchanged priority: clean GC completion over fast shutdown» (`:889-890`).
2. «whole in-flight unbounded scans» — НЕТОЧНО. Раунд ограничен `GcRoundWorkBudget` (`Gc/CasBlobInDegree.h:251-291`), один экземпляр на весь раунд, заполняемый из настроек пула в `Gc/CasGc.cpp:539-548`; дефолты не нулевые: `gc_round_graduation_budget=5000`, `gc_round_redelete_budget=5000`, `gc_round_sweep_namespace_budget=20`, `gc_round_sweep_recovery_op_budget=5000`, `gc_round_ref_cleanup_budget=5000`, `gc_round_prefix_wholesale_budget=20000`, `gc_round_outcome_entry_budget=5000` (`ContentAddressedSettings.cpp:76-83`). То есть деструктивная и recovery-работа раунда ограничена по числу операций; неограниченным остаётся только время (нет ни одного тайм-бюджета: `grep -rn "deadline\|time_budget\|max_duration" ContentAddressedSettings.cpp` — пусто) и объём LIST, пропорциональный размеру пула.
3. «оператор не может остановить GC» — форма избыточно резкая: `GC STOP` не отказывает и не зависает навсегда, он ЖДЁТ окончания раунда, ограниченного бюджетами; при медленном бакете ожидание может быть длинным, но конечным. Данные-пути (`poolAccess`, чтение/запись, `gcHealth`) `lifecycle_mutex` не берут — комментарий `:432` специально требует, чтобы снапшот «NEVER wait behind gc_scheduler_mutex». Блокируются только операторские verbs, не запросы.
4. Класс — LIVENESS операторской плоскости, fail-open по данным (ничего не портится, ничего не теряется), поэтому не P1.

**BACKLOG / история.** Частичное покрытие есть: `docs/superpowers/cas/BACKLOG/gc.md:63` `{#fsck-scale-timeout}` — измеренный факт, что `ca-fsck` не укладывается в свой дедлайн на ~29-31 GiB, и направление «bounded/streamed partial verdicts … deadline-aware partial reporting via the existing `partial`/`partial_reason` fields»; также `operability-and-introspection.md:111` `{#fsck-large-pool-fixed}` (пункт (c) — fsck не заканчивается на 5.5 GB в 180s). Но именно про (а) отсутствие кооперативной отмены раунда/скана и (б) то, что SQL-вход FSCK не передаёт `deadline`/`partial` и не убивается `KILL QUERY`, — записи не было. `docs/superpowers/cas/2031-triage.md` этого сюжета не содержит. Добавлен (uncommitted) раздел `docs/superpowers/cas/BACKLOG/operability-and-introspection.md` → `{#lifecycle-verbs-wait-out-uncancellable-scans}`.

**Что реально осталось.** Два конкретных owed-пункта, оба операбельность: (1) кооперативная отмена GC-раунда, чтобы `GC STOP`/`FORGET`/`shutdown` не ждали полного прохода по медленному бакету; (2) SQL FSCK должен передавать deadline (из `max_execution_time`) и `partial_on_deadline`, а также проверять отмену запроса, чтобы `KILL QUERY` работал и `lifecycle_mutex` не держался неограниченно долго. P2, пост-релизно.

## CAS-050 — Гонка данных на объектах потоков в `CasGcScheduler::stop` подтверждена (join вне `mutex`, достижимо через `SYSTEM CAS DROP POOL MEMBER` параллельно с GC STOP/shutdown), но вторая половина находки — «joinable-but-dead планировщик, который сообщает, что работает» — практически не воспроизводится и вредом не является. (частично, P2) {#cas-050}

Файл переехал: анкер находки `CA/Gc/CasGcScheduler.cpp:67-79` устарел (перемещение `592b9b83568`, «git mv only»); на HEAD это `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp`, номера строк сдвинулись: `stop()` — 75-93, `requestRoundSoon()` — 95-104, `start()` — 65-73, терминальные самовыходы — 281-291 (`loop`) и 369-378 (`heartbeatLoop`).

1) «join вне мьютекса, который охраняет объекты потоков» — ПОДТВЕРЖДЕНО как форма кода.
`CasGcScheduler.h:196-204` объявляет `std::mutex mutex; ... ThreadFromGlobalPool thread; ... ThreadFromGlobalPool hb_thread;` — `thread`/`hb_thread` создаются и присваиваются строго под `mutex` в `start()` (`CasGcScheduler.cpp:67-72`) и читаются под `mutex` в `requestRoundSoon()` (`CasGcScheduler.cpp:98-101`, `if (stopping || !thread.joinable()) return;`). А `stop()` берёт `mutex` только для выставления `stopping` (`CasGcScheduler.cpp:77-80`) и затем обращается к объектам потоков БЕЗ него: `CasGcScheduler.cpp:82-85`
`if (thread.joinable()) thread.join(); if (hb_thread.joinable()) hb_thread.join();`.
Это настоящая гонка данных, а не формальность: `ThreadFromGlobalPool::join()` делает `state.reset()` (`src/Common/ThreadPool.h:390-400`), т.е. ПИШЕТ в член-`shared_ptr`, а `joinable()` (`src/Common/ThreadPool.h:410`) этот же `shared_ptr` читает. Одновременный `reset()` и чтение одного и того же `shared_ptr` — UB (и стабильный отчёт TSan), в худшем случае разрушение контрольного блока.

2) Достижимость — ПОДТВЕРЖДЕНА, но узкая: только `requestRoundSoon` против `stop`.
Все вызовы `start()`/`stop()` на опубликованном планировщике взаимно сериализованы `gc_scheduler_mutex` (плюс `lifecycle_mutex` у verbs): `gcStop` — `ContentAddressedMetadataStorage.cpp:1109-1146` (в приведённой нумерации файла: `std::lock_guard lifecycle(lifecycle_mutex); std::lock_guard round_lock(gc_scheduler_mutex);` перед `snapshot->stop()`), `gcStart` — там же перед `snapshot->start()`, `shutdown()` — `std::lock_guard round_lock(gc_scheduler_mutex);` перед `old_scheduler->stop()`, `forgetDisk()` — `lifecycle_mutex` + `gc_scheduler_mutex` перед `pool->forgetDisk([&scheduler]{ scheduler->stop(); }, reason)`. `start()` на пути `startup()` вызывается на ЛОКАЛЬНОМ `shared_ptr` до публикации (`ContentAddressedMetadataStorage.cpp:857-863`), т.е. объект ещё никому не виден. Значит заявленная гонка `stop()` против `start()` фактически закрыта внешней сериализацией.
Чего внешняя сериализация НЕ закрывает: `ContentAddressedMetadataStorage::requestGcRoundSoon()` (`ContentAddressedMetadataStorage.cpp:419-428`) берёт ТОЛЬКО `pointer_mutex` для снимка `shared_ptr` и затем вызывает `snapshot->requestRoundSoon()` вне всяких lifecycle-локов. Единственный вызывающий — `SYSTEM CAS DROP POOL MEMBER` (`src/Interpreters/InterpreterSystemQuery.cpp:1077`, коллбэк `decommissionPoolMember`). Так как `gcStop` намеренно ОСТАВЛЯЕТ планировщик в члене (копия, а не `std::move` — `ContentAddressedMetadataStorage.cpp:985-990`), окно открыто целиком: `DROP POOL MEMBER` + одновременный `SYSTEM CAS GC STOP` (или штатный `shutdown()`) даёт `joinable()` под `mutex` из одного потока против `join()` без `mutex` из другого. Это реально, требует одновременных операторских действий, и не является тихой порчей данных — но это UB и почти наверняка красный TSan.
Починка тривиальна и не меняет протокол: в `stop()` под `mutex` переместить объекты потоков в локальные переменные (или добавить отдельный `join_mutex`), join делать снаружи.

3) «Потоки самовыходят независимо, оставляя joinable-но-мёртвый планировщик, который сообщает, что работает» — В ОСНОВНОМ НЕ БАГ (типичный для этого аудита приём: верная форма кода + недостижимое следствие).
Форма верна: `loop()` (`CasGcScheduler.cpp:281-291`) и `heartbeatLoop()` (`CasGcScheduler.cpp:369-378`) действительно возвращаются сами, и завершившийся `ThreadFromGlobalPool` остаётся `joinable()` до `join()`, поэтому `start()` (`CasGcScheduler.cpp:68-69`) станет no-op, а `requestRoundSoon` будет считать планировщик живым.
Но следствия нет:
- Самовыход происходит ТОЛЬКО при `store->isVanished() || store->vanishedIntentPublished() || lifecycle() == IdentityLost` (`CasGcScheduler.cpp:281-282`, `369-370`), т.е. на терминальном (или уводимом в FORGET) пуле, где перезапуск GC бессмысленен по определению;
- `gcStart()` на таком пуле вообще не доходит до `start()`: `checkOpAdmitted(CasOpClass::Admin)` (`ContentAddressedMetadataStorage.cpp:1013` в приведённом фрагменте) отказывает на transient/`IdentityLost`/`Vanished` — отказ громкий и типизированный, а не тихий;
- «сообщает себя работающим» неверно по фактам: публичного «is running» нет, а `GcHealth::is_leader` перед самовыходом явно сбрасывается (`CasGcScheduler.cpp:284`, `i_am_leader.store(false)`), так что `system.cas_mounts` показывает «не лидер»;
- остаточное окно `vanishedIntentPublished()` без терминального lifecycle (FORGET в полёте) закрывается самим FORGET, который в любом случае вызывает `stop()`+join.
Таким образом второй заявленный ущерб — по сути fail-closed/безвредный.

BACKLOG и история. Существующего покрытия именно этой гонки нет. Ближайшее — `docs/superpowers/cas/BACKLOG/mounts-and-lifecycle.md:50-52`, пункт (c) в разделе {#disk-lifecycle-rev8-closure}: «GC `start()` partial-start desync guard (pre-existing)» — DEFERRED, про несогласованность полустарта, а не про join вне мьютекса; и `mounts-and-lifecycle.md:101` {#orphan-triage-2026-08-04} `[gc-scheduler-lazy-init-race]`. Про последний отмечу отдельно: на HEAD он ЗАКРЫТ — ленивое создание `gc_scheduler` в `runOneGcRoundForTest` и `runGarbageCollectionRoundNow` идёт под `std::lock_guard ptr_lock(pointer_mutex)` внутри `gc_scheduler_mutex` (`ContentAddressedMetadataStorage.cpp:399-415` и `629-643`); переход на этот вид сделан коммитами `452d17af42f` («partAccess returns a shared-ownership snapshot; gcHealth no longer blocks behind a GC round») и `e79a109b142` («GC STOP/START verbs — restartable scheduler»). Сам шаблон `if (thread.joinable()) thread.join();` в `stop()` в истории не менялся — `git log -S 'if (thread.joinable())'` даёт только `592b9b83568` (перемещение файла), т.е. дефект существует с рождения планировщика и ничем не закрыт.

Реальный остаток (внесён в backlog, uncommitted): гонка `stop()` vs `requestRoundSoon()` на `thread`/`hb_thread`. Добавлен раздел `docs/superpowers/cas/BACKLOG/mounts-and-lifecycle.md` {#gc-scheduler-stop-join-race}. P2: UB/красный TSan в узком операторском окне, тривиальная починка, без тихой порчи данных и без потери данных — не блокер релиза.

## CAS-051 — Утечка счётчика `pending_snapshot_publishes` при провале dispatch — исправлена ещё `829ad698ef6`, поэтому «вечное ожидание» в `quiesceRefTablesForRemount`/`dropNamespaceImpl` недостижимо; подтверждается только вторая половина: fan-out фоновых публикаций не ограничен пулом-широко (по одному потоку на namespace). (частично, P2) {#cas-051}

Устаревшие анкеры. Находка ссылается на `CA/Pool/CasRefLedger.cpp:2754-2783`; на HEAD файл — `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` (перемещение `592b9b83568`), а строки другие: `admitSnapshotPublishUnderStateLock` — 3957-3992 (инкремент на 3987), `dispatchSnapshotPublisher` — 3994-4033, `settleSnapshotPublish` — 4035-4060, `maybeScheduleSnapshotPublish` — 4062-4079; ожидания — `quiesceRefTablesForRemount` `CasRefLedger.cpp:1709-1714` и `dropNamespaceImpl` `CasRefLedger.cpp:5105-5111`.

1) «dispatch может потерять pending count после инкремента» — ФАКТИЧЕСКИ НЕВЕРНО на HEAD (закрыто более ранней работой).
Инкремент делается ровно в одном месте, под `rt.state_mutex`: `CasRefLedger.cpp:3987` `rt.pending_snapshot_publishes.fetch_add(1, ...)` внутри `admitSnapshotPublishUnderStateLock`. Единственный бросающий шаг после него — конструктор `ThreadFromGlobalPool` в `dispatchSnapshotPublisher`, и он обёрнут:
`CasRefLedger.cpp:4019-4032`
```
catch (...)
{
    /// The `ThreadFromGlobalPool` ctor can throw (pool exhaustion) AFTER the count was incremented.
    {
        std::lock_guard lock(rt->state_mutex);
        rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
    }
    rt->publish_settle_cv.notify_all();
    tryLogCurrentException(...,"CAS background snapshot-publish dispatch failed to launch");
}
```
То есть счётчик откатывается и `publish_settle_cv` пробуждается. `git log -S 'snapshot-publish dispatch failed to launch'` показывает, что этот блок добавлен коммитом `829ad698ef6` (2026-07-12, «cas: monotonic snapshot adoption…», в диффе прямо помечено «Review follow-up (T11): … Undo the count (else `waitForSnapshotPublishSettleForTest` hangs and the leaked pending count wedges every later settle)»); при извлечении `CasRefLedger` (`636d0445791`) блок перенесён без изменений. Тем самым сценарий аудита был закрыт ещё ДО снимка, по которому он писался.
Других путей утечки нет: `maybeScheduleSnapshotPublish` (`:4072-4078`) вызывает dispatch немедленно после успешного admit, без промежуточных бросающих шагов; `settleSnapshotPublish` (`:4052-4057`) декрементирует и повторно admit'ит под ОДНИМ удержанием `state_mutex`, а неудачный redispatch снова гасится тем же catch'ем; в самом потоке тело обёрнуто try/catch и `settleSnapshotPublish` вызывается безусловно (`:4008-4016`).

2) «два неограниченных ожидания зависнут навсегда» — форма верна, следствие недостижимо.
Оба ожидания действительно без таймаута: `CasRefLedger.cpp:1710-1713` `rt->publish_settle_cv.wait(slock, [&]{ return rt->pending_snapshot_publishes.load(...) == 0; });` и `CasRefLedger.cpp:5106-5111` то же в `dropNamespaceImpl`. Но зависание требует именно утечки счётчика из п.1, которой нет. Дополнительно оба места защищены от «движущейся цели»: новые dispatch'ы подавлены fence-гардом `if (!may_mutate()) return;` (`:4069-4070`) и флагами `superseded_by_remount`/`catalog_life_invalidated`, которые `admitSnapshotPublishUnderStateLock` проверяет первыми (`:3966-3967`), так что settle не может пере-диспетчить публикацию во время дренажа. Отдельного дефекта здесь нет.

3) «fan-out не ограничен пулом-широко» — ПОДТВЕРЖДЕНО.
Гейт single-in-flight — строго ПО ТАБЛИЦЕ: `CasRefLedger.cpp:3971` `rt.pending_snapshot_publishes.load(...) == 0` читает per-runtime счётчик (`CasRefLedger.h:826`), а глобального счётчика/семафора/выделенного пула нет — `dispatchSnapshotPublisher` каждый раз создаёт detached `ThreadFromGlobalPool` (`:4005-4017`). Порог тоже per-namespace и низкий: `snapshot_log_count_threshold = 256`, `snapshot_log_bytes_threshold = 1 MiB` (`Pool/CasPool.h:234-235`, `Pool/CasRefProtocol.h:158-159`). Волна вставок, пересекающая порог на N таблицах одного пула, действительно порождает N одновременных полно-namespace перекодирований + условных PUT на глобальном пуле.
Однако это ресурсно-производственный дефект, а не корректностный и не тихий: исчерпание глобального пула фейлится мягко и по правильной стороне (catch из п.1 — счётчик откачен, ошибка залогирована, публикация переносится на следующий триггер), а per-table backoff (`advancePublishBackoff`, `:4082-4092`, проверяется в admit на `:3974`) гасит PUT-штормы после недоставленных публикаций. Верхняя граница — число namespace'ов, т.е. по факту число CA-таблиц, а не бесконечность. Разумная починка: общий на пул ограничитель параллельных snapshot-публикаций (счётчик/`ThreadPool` с конфигурируемым `max`), с сохранением per-table single-flight.

BACKLOG. Существующего покрытия нет: в `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` про snapshot-публикации есть только `docs-and-cleanup.md:55` `[snappatch-minor]` (replay-throw без арминга backoff, MINOR/defensive) и `performance.md:111` `[ref-table-copy-commit-path]` — оба про другое. Реальный остаток (п.3) внесён новым разделом в `docs/superpowers/cas/BACKLOG/performance.md` {#snapshot-publish-fanout-unbounded} (uncommitted). P2 — трекать после релиза: деградация под многотабличной нагрузкой, fail-soft, без потери данных.

## CAS-052 — Форма кода описана верно, но следствие недостижимо: `Pool` всегда живёт под `shared_ptr`, все синхронные вызовы приходят от владельцев `PoolPtr`, а detached-поток пинит пул копией `self`. (not-a-bug, —) {#cas-052}

Якорь устарел по номерам строк (файлы переехали в подкаталоги коммитом 592b9b83568). На HEAD код находится в `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1451-1516` (`Pool::reportImpossibleInterference`), `shared_from_this()` — на `CasPool.cpp:1478`, `try`/`ThreadFromGlobalPool(...).detach()` — на `:1479-1510`, `catch (...)` — `:1511-1515`.

1) «`shared_from_this()` стоит вне `try`» — ПОДТВЕРЖДАЕТСЯ как факт о форме кода: `CasPool.cpp:1478` действительно вне `try`, который открывается только на следующей строке (`:1479`). Но `try` там намеренно узкий: его единственная задача — проглотить исключение конструктора `ThreadFromGlobalPool` при исчерпании пула потоков (комментарий `CasPool.cpp:1512-1513`). Это осознанная граница, а не пропущенная защита.

2) «может выбросить `bad_weak_ptr`» — НЕДОСТИЖИМО. `bad_weak_ptr` из `enable_shared_from_this` возможен только если объект не принадлежит `shared_ptr` (создан на стеке/через `new` без shared-владения) либо вызов идёт из деструктора/после обнуления счётчика владения. Ни одно из этого не выполняется:
   - конструктор `Pool` приватный (`CasPool.h:791-796`), а оба фабричных пути создают именно `shared_ptr`: `CasPool.cpp:501` и `CasPool.cpp:846` (`PoolPtr store(new Pool(...))`, `using PoolPtr = std::shared_ptr<Pool>` — `CasPool.h:316`). Стековых `Pool` в дереве нет, тесты тоже идут через `Pool::open` (например `src/Disks/tests/gtest_cas_ref_read_contract.cpp:117`);
   - единственная точка вызова — callback ledger'а, установленный в конструкторе `Pool` (`CasPool.cpp:196-197`), и вызывается он только из runtime-путей записи: `CasRefLedger.cpp:2524` (разбор wedge), `:3317` (не-`Ready` состояние линии перед выделением id), `:3673` (чужой объект на выведенном id). Все три сидят в `resolveWedgeOnce`/`flushRefBatch`, то есть внутри `appendRefOps*`, вызываемого держателем `PoolPtr` (`ContentAddressedMetadataStorage.h:635` `cas_store`, `CasPartWriteTxn.h:363` `store`, `CasGc.h:896` `store`, `CasGcScheduler.h:179` `store`, `PartFolderAccess.h:365` `store`). Пока такой вызов идёт, счётчик владения > 0;
   - из `~Pool` (`CasPool.cpp:861-889`) этот callback не достигается: единственный шаг деструктора, трогающий ledger, — `ref_ledger.drainRefLanesForShutdown` (`CasRefLedger.cpp:1854-1912`), который только защёлкивает `shutting_down`, ждёт `cv` до дедлайна и проверяет `lane_state`; он не делает ни одного PUT/CAS и ни разу не вызывает `on_impossible_interference`. Новые аппенды на этом этапе уже отвергаются (`CasRefLedger.cpp:1973-1976`);
   - все асинхронные пути ledger'а пинят владельца через `pin_owner()` == `Pool::shared_from_this` (`CasPool.cpp:198`, использование — `CasRefLedger.cpp:3994-3999`), поэтому фоновый publisher не может работать одновременно с `~Pool`. Это инвариант, явно зафиксированный в комментарии `CasPool.h:1132-1135`.

3) «передаёт detached-потоку разрушающийся пул» — ФАКТИЧЕСКИ НЕВЕРНО. Лямбда захватывает `self` ПО ЗНАЧЕНИЮ (`CasPool.cpp:1481`: `ThreadFromGlobalPool([self, key]`), то есть поток держит собственный `shared_ptr` и продлевает жизнь пула на всё своё время (ровно та же схема, что у `dispatchSnapshotPublisher`, `CasRefLedger.cpp:3996-3999`). Более того, порядок строк исключает и «висячий поток»: `shared_from_this()` выполняется ДО создания потока, так что гипотетический бросок вообще не дал бы создать поток.

4) Класс отказа. Даже в гипотетическом сценарии это был бы громкий отказ, а не тихая порча: реакция fail-closed (`mount_runtime.tripMountLost()` + `scheduleRemount()`, `CasPool.cpp:1470-1471`) выполняется ДО строки 1478, то есть предохранительное действие уже совершено; исключение из диагностики лишь подменило бы уже готовое `CORRUPTED_DATA` вызывающей стороны (`CasRefLedger.cpp:2528-2533`, `:3679-3688`) на другое исключение того же fail-closed пути.

История/BACKLOG: пин `auto self = shared_from_this()` существует с момента введения политики аномалий (`f971c0c27d9` «cas: rev.6 task 11 — wedge hard contract + anomaly policy (ForeignInterference)»), в текущий файл попал переименованием `b79fbd83909`. В BACKLOG упоминание `reportImpossibleInterference` есть только как кандидат на будущее вынесение компонента — `docs/superpowers/cas/BACKLOG/docs-and-cleanup.md` {#source-layout-casstore-followups}, к этой находке отношения не имеет. Тесты `src/Disks/tests/gtest_cas_ref_writer.cpp:1891-1899`, `:1957-1965` уже прижимают, что путь реально вызывает `scheduleRemount` ровно один раз, то есть сам вызов проходит без исключения.

Итог: остатка нет, новый пункт в BACKLOG не добавлялся.

## CAS-053 — Единственная точка вызова и незащищённое вычитание подтверждаются, но «раз на namespace и больше никогда» неточно (пасс идёт на каждое холодное admission), а последствия — только лишний recovery-I/O и мягкий, а не жёсткий, потолок памяти; корректность не страдает. (частично, P3) {#cas-053}

Якоря устарели по номерам (переезд файлов коммитом 592b9b83568). На HEAD: `enforceRefTableCacheBudget` — `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1611-1690`, единственный вызов — `:1550`, skip-предикаты — `:1650` и `:1659`, `ref_table_cache_bytes = 256ULL << 20` — `Pool/CasPool.h:265` (плюс дефолт-дубль в `Pool/CasRefProtocol.h:164`).

1) «Единственная точка вызова» — ПОДТВЕРЖДАЕТСЯ: `grep` по всему дереву даёт ровно одно обращение (`CasRefLedger.cpp:1550`), в конце `ensureRefTableRecovered` (`:1325`).

2) «Бюджет проверяется один раз на namespace recovery и больше никогда» — ЧАСТИЧНО НЕВЕРНО в формулировке, но верно по сути гэпа. Пасс выполняется не «однажды», а при каждом фактическом recovery: тёплое касание выходит раньше на `CasRefLedger.cpp:1339` (`if (rt.recovered && !needs_rerecovery()) return;`), а холодное/повторное (`NeedsRecovery`) доходит до `:1550`. То есть это классическая LRU-политика «вытесняем на вставке»: каждое admission новой таблицы в кэш прогоняет вытеснение. Реальный, не описанный аудитом дефект — отсутствие ре-энфорсмента при РОСТЕ уже резидентных таблиц: `tail_bytes_since_snapshot` растёт на каждой применённой транзакции (`:2470`, `:3791`), `base_snapshot_bytes` обновляется на recovery/публикации (`:1568`), но стабильный набор таблиц, который пишут вечно и который ни разу не перечитывается «с холода», не запускает пасс вообще. Формула веса — `base_snapshot_bytes + tail_bytes_since_snapshot` (`:1627-1629`, комментарий `CasRefLedger.h:797-807`).

3) «Не может вытеснить таблицу, в которую пишут» — ПОДТВЕРЖДАЕТСЯ, но это BY DESIGN и по причинам корректности, а не производительности: гейт `rt.use_count() != 1 || rt->leader_active || !rt->pending.empty()` (`:1650`, повторная проверка `:1659`) — именно то, что делает невозможным split-brain линии аппенда (комментарий `:1641-1648`), а wedged-линия не вытесняется потому, что её незавершённый in-flight PUT не восстановим из durable-объектов (`:1668-1676`, проверка `lane_state != RefLaneState::Ready` на `:1678`). То же зафиксировано в документации поля (`CasPool.h:255-265`: «эффективный пол — одна таблица»). Тесты прижимают контракт: `src/Disks/tests/gtest_cas_ref_writer.cpp:2025` (`WholeTableEvictionUnderBudgetReRecovers`), `:2054` (`ZeroBudgetDisablesEviction`), `:2067` (`WedgedTableIsNeverEvicted`).

4) «Арифметика может уйти в underflow и вытеснить все вытесняемые таблицы сразу» — ПОДТВЕРЖДАЕТСЯ как возможность, с сильно ограниченным следствием. `total -= c.weight` на `:1667` — беззнаковое вычитание без клампа; `total` набирается в первом проходе (`:1634-1636`) по ВСЕМ таблицам, включая горячие, чьи атомики мутируются под `state_mutex`, а не под удерживаемым здесь `ref_queue_mutex` (кросс-локовое чтение прямо признано в комментарии `:1622-1626` и `CasRefLedger.h:790-793`), тогда как `c.weight` берётся позже, во втором проходе (`:1657`). Таблица, горячая на момент первого прохода и ставшая idle ко второму, может дать вес больше того, который вошёл в `total`; если приращение превысит суммарный вес всех прочих таблиц, `total` уйдёт в underflow, условие `total <= config.ref_table_cache_bytes` (`:1662`) останется ложным и цикл выселит все idle-`Ready`-таблицы за один проход. Достижимость узкая (нужно, чтобы лидер флаша успел отпустить свою копию `shared_ptr` между проходами и чтобы прирост перевесил весь остальной кэш), но не нулевая. Класс отказа — лишний recovery-I/O, самоограничивающийся (кандидатов конечное число), без потери данных: выселение разрешено только для idle/`Ready`/не-wedged. Показательно, что кламп для ровно этого класса в файле уже есть — `clampedCounterSub` (`:4191-4198`), применяется на `:4423-4424` с комментарием про underflow, но на `:1667` не используется.

5) «256 MiB hardcoded» — ПОДТВЕРЖДАЕТСЯ и это самый предметный остаток. `ref_table_cache_bytes` — единственный кэш-бюджет `PoolConfig` без записи в `ContentAddressedSettings`: `deduplication_cache_bytes` объявлен в `ContentAddressedSettings.cpp:70` и прокидывается в `ContentAddressedMetadataStorage.cpp:759`, `manifest_decode_cache_bytes` — `ContentAddressedSettings.cpp:90` и `ContentAddressedMetadataStorage.cpp:761`, а `ref_table_cache_bytes` вне gtest'ов не выставляется нигде (grep по `src/` даёт только объявления и три чтения в самом ledger'е). Плюс нет gauge: в `src/Common/CurrentMetrics.cpp` метрики ref-таблиц нет, есть только `ProfileEvents::CASRefTableEvictions` (`src/Common/ProfileEvents.cpp:788`). Оператор не может ни настроить, ни наблюдать этот кэш.

BACKLOG: существующего покрытия не нашлось — grep по `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` по `ref_table_cache`/`ref-table cache`/`eviction` даёт только `testing-and-ci.md` {#gate-filter-gap-3-backend-contract} (про gtest-фильтр, скрывавший сюиту `RefTableCacheEviction`), к самому бюджету отношения не имеющий. Поэтому добавлен новый раздел (незакоммиченный) `docs/superpowers/cas/BACKLOG/performance.md` {#ref-table-cache-budget-admission-only} с тремя остатками: кламп вычитания, вынос бюджета в `ContentAddressedSettings` + gauge, и второй, «рост-ориентированный» триггер пасса (например, после публикации снапшота).

Оценка: P3. Тихой порчи нет (вытеснение гейтится корректностью), потолок памяти мягкий и снизу ограничен реальным размером ref-карт активно пишущихся таблиц, которые всё равно обязаны быть резидентными; underflow даёт всплеск recovery-I/O, а не потерю. Пре-релизного гейта не требует.

## CAS-054 — Главное обвинение (O(R) пере-кодирование в `admits`) устарело — закрыто ещё в июле; снапшот раз в 256 транзакций — это by-design чекпойнт в фоновом потоке; реально остался только debug/sanitizer-ассерт, возвращающий O(K·N) на replay. (частично, P3) {#cas-054}

**Разбор по пунктам.**

1) «`admits()` пере-кодирует всю ref-таблицу на каждую растящую состояние операцию» — **устарело, закрыто**. На HEAD `admits` (`Pool/CasRefProtocol.cpp:718-736`) читает инкрементально поддерживаемые счётчики: `encodedSnapshotBudgetSize`/`encodedRemovalBudgetSize` (`:705-715`) складывают framing с `state.getSnapshotBodyBytes()`/`getRemovalBodyBytes()`, а не кодируют таблицу. Счётчики `snapshot_body_bytes`/`removal_body_bytes` объявлены в `Pool/CasRefProtocol.h:262-263` и обновляются дельтами в каждом `applyOp`-плече (`Pool/CasRefProtocol.cpp:262-263, 275-276, 291-292, 306-307, 326-327, 355-358`). Закрывающие коммиты: `13ab814869c` («cas: maintain incremental body-byte counters on RefTableState (debug drift chassert)») и `b5f448e9b41` («cas: rewrite admits() to O(1) via incremental budget-size accessors»). Замеры зафиксированы в шапке бенчмарка `benchmarks/benchmark_cas_ref_protocol.cpp:20-27`: до фикса N=100 → 48.8 мкс, N=100 000 → 55 976 мкс (фит O(N log N)); после — 1842 нс … 1919 нс, фит O(1). Там же `:48-53` и `:75-79`: линейный скан `manifestAlreadyOwned` в плече add-precommit (400 мкс при N=100k) заменён индексом `owned_manifests` и стал ~692-714 нс flat (≈571× при N=100k). То есть ровно эта формулировка аудита в самом дереве помечена как «(now RESOLVED)» (`benchmarks/benchmark_cas_ref_protocol.cpp:13-15`).

2) «Размеры строк получаются пере-сериализацией строк, два полных кодирования строки на ref-операцию» — **факт верен, но следствие неверно масштабировано**. `committedRowEncodedSize`/`precommitRowEncodedSize` (`Formats/CasRefSnapshotFormat.cpp:278-289`) действительно прогоняют ОДНУ строку через `CasJsonWriter(256)`, и `removalOpEncodedSize` — аналогично. Но это O(1) на операцию, а не O(R): суммарный размер тела держится дельтами. Это осознанный обмен, задокументированный в `Formats/CasRefSnapshotFormat.h:84-86` («Reuses the same writer, so it is byte-identical to that row's contribution to a full encode») и в `Pool/CasRefProtocol.cpp:471-474` — байт-точность вместо «drift-prone estimate», что и есть причина, по которой бюджет допуска можно считать инкрементально. Цена — единицы наносекунд на операцию (для сравнения, полный `encodeRefTableSnapshot` идёт ~159 нс/строка, `benchmarks/benchmark_cas_ref_protocol.cpp:62-64`). Не дефект.

3) «Публикация ref пере-кодирует весь namespace каждые 256 транзакций» — **форма кода верна, следствие — by-design, и не на горячем пути**. Порог: `Pool/CasRefProtocol.h:158-159` (`snapshot_log_count_threshold = 256`, `snapshot_log_bytes_threshold = 1 MiB`), проброшен из `Pool/CasPool.h:234-235, 277-278`. Проверка порога — `Pool/CasRefLedger.cpp:3983-3984`, и она читает инкрементальные хвостовые счётчики (`tail_count_since_snapshot`/`tail_bytes_since_snapshot`, `:3980-3982`) — «no walk, no age filter». Сама публикация уходит в отдельный поток: `dispatchSnapshotPublisher` (`Pool/CasRefLedger.cpp:3994-4032`) запускает `ThreadFromGlobalPool` с `ThreadName::CAS_REF_SNAPSHOT_PUBLISH` и в комментарии прямо сказано «Off the mutation hot path». Есть single-in-flight gate и backoff-дедлайн (`admitSnapshotPublishUnderStateLock`, `:3966-3975`), то есть PUT-штурма нет. Снапшот — это и есть механизм, ограничивающий длину replay-хвоста (`Pool/CasRefProtocol.h:187-190`: `TableState = Replay(S_X.state, tail(X))`); убрать его нельзя, вопрос только в настройке порога, и порог — настраиваемый. Амортизированная цена при R=100k рефов: 15.9 мс на снапшот (159 нс/строка) на 256 коммитов ≈ 62 мкс на коммит, в фоне. Это не баг.

4) «В debug и sanitizer сборках каждый apply транзакции и каждый `admits()` превращаются в дополнительное O(R) пере-кодирование» — **подтверждено на HEAD, и это единственный живой остаток**. `RefTableState::debugAssertBodyCounters` (`Pool/CasRefProtocol.cpp:483-504`) под `#ifdef DEBUG_OR_SANITIZER_BUILD` полностью пересчитывает оба счётчика и членство `owned_manifests`: два пере-кодирования строки на каждый committed-реф и два на каждый precommit. Вызывается в конце каждого `applyTxnInPlace` (`Pool/CasRefProtocol.cpp:583-586`) и на scratch-состоянии каждого `admits`-превью (`:729-731`).

Существенный нюанс, которого в самом finding нет: этот ассерт **инвертирует задокументированную сложность** ровно для тех сборок, где гоняются soak и correctness-прогоны. Комментарий на месте in-place apply (`Pool/CasRefProtocol.cpp:575-578`) говорит, что replay на K транзакций над базой из N строк был осознанно снижен с `O(K*N)` до `O(K + N)` — а per-apply полный пересчёт возвращает `O(K*N)` в debug/ASan/TSan. `replay` доходит до `applyTxnInPlace` напрямую (там же), так что путь восстановления затронут именно так.

При этом это **fail-loud инвариантная защита**, не молчаливая порча: `chassert` на расхождении счётчиков. Удалять её нельзя — она и есть то, что делает инкрементальные счётчики доказуемо байт-точными (`Pool/CasRefProtocol.cpp:471-474`). Поэтому приоритет низкий: релизная сборка не затронута вообще, речь только о пропускной способности sanitizer-прогонов. Разумный фикс — сохранить проверку, но семплировать её на `applyTxnInPlace` (например, только на первом apply после install либо под тестовым флагом), оставив на `admits`-превью, которые и так single-op.

**BACKLOG.** Существующего покрытия для п.4 не нашлось: `docs/superpowers/cas/BACKLOG/ref-protocol.md:42-45` («Recovery re-runs 3-4 codec passes per snapshot row») — про кодек-проходы на восстановлении по снапшоту, а не про debug-ассерт; `docs/superpowers/cas/BACKLOG/performance.md:111` (`[ref-table-copy-commit-path]`) — про КОПИЮ состояния на commit-пути, не про кодирование. Историческая запись «admits() re-encodes the WHOLE ref table once per state-growing op» действительно существовала и уже помечена RESOLVED в шапке бенчмарка. Добавлена новая секция в `docs/superpowers/cas/BACKLOG/ref-protocol.md`, анкер `{#debug-body-counter-assert-on-replay}` (не закоммичена).

**Итог.** Из четырёх утверждений одно устарело (закрыто `13ab814869c` + `b5f448e9b41`), одно верно по форме но безобидно по величине, одно — by-design фоновый чекпойнт с single-flight и backoff, одно подтверждено и оформлено как P3-остаток. Ничего перед релизом делать не нужно.

## CAS-055 — Подтверждено: ветка carry-forward в `createHardLink` делает ForceFresh-resolve на каждый файл, а при дефолтном `part_folder_validate = always` это обязательный `HEAD` манифеста на файл; но «полная пересборка view» преувеличена (декод берётся из кэша), а фикс — мемоизация уровня транзакции, уже существующая в `unlinkFile`. (подтверждено, P2) {#cas-055}

**Цепочка подтверждена целиком, по звеньям.**

1) Место вызова. `ContentAddressedTransaction::createHardLink`, ветка «carry forward from the COMMITTED source part»: `auto view = metadata_storage.partAccess()->getView(src->refKey(), Cas::Freshness::ForceFresh);` — `ContentAddressedTransaction.cpp:1190` (анкер аудита `CA/ContentAddressedTransaction.cpp:816` устарел по номеру строки; файл переехал вместе с реорганизацией `592b9b83568`, но symbol на месте). Ветка со staged-источником (`:1161-1184`) в объектное хранилище не ходит вовсе — только staged-источник в этой же транзакции; амплификация касается именно committed-источника.

2) Дефолт настройки. `DECLARE(String, part_folder_validate, "always", "ForceFresh body re-proof policy (always | never | age <seconds>)", 0)` — `ContentAddressedSettings.cpp:89`. Заявлено как fail-closed дефолт в `Parts/PartFolderAccess.h:240`.

3) Кэш действительно отключён для этого режима. Short-circuit по retained view для `ForceFresh` явно закрыт условием `params.validate.mode != PartFolderValidate::Mode::Always` — `Parts/PartFolderAccess.cpp:197`. Значит при дефолте `getView(..., ForceFresh)` всегда доходит до `buildView` (`:215`), а `buildView` для не-`CachedForLoad` режимов даже не участвует в single-flight: «Fresh modes do not coalesce: each `ForceFresh`/`StrictValidate` call owns its mandatory HEAD» (`Parts/PartFolderAccess.cpp:267-270`).

4) Цена одного вызова. `buildView` вызывает `store->readManifestShared(resolved.manifest_id)` (`Parts/PartFolderAccess.cpp:270`), а там `HEAD` обязателен даже при попадании в кэш декодов: «`HEAD` is mandatory even on a cache hit» — `Pool/CasManifestReader.cpp:63-65`. Проверка кэша декодов идёт ПОСЛЕ `HEAD` (`:83-85`), `GET` — только при промахе по паре `(ManifestId, Token)` (`:87-91`). Сам `resolve` — это `store->resolveRef(...)` (`Parts/PartFolderAccess.cpp:316-319`), то есть чтение ref-таблицы в памяти смонтированной таблицы, без сетевого запроса.

Отсюда **корректировка величины из finding'а**: «one manifest HEAD plus one full view rebuild per unchanged file» — `HEAD` реален и повторяется на каждый файл, а вот «full view rebuild» преувеличено. `PartFolderView::make` (`Parts/PartFolderAccess.cpp:64-71`) лишь оборачивает `shared_ptr<const PartManifest>`, повторного декода нет, пока токен манифеста не сменился. То есть цена = N сетевых `HEAD` на часть из N файлов, без N `GET` и без N декодов. Это всё равно материально (широкая часть на 30 колонок ≈ 240 файлов ⇒ ≈240 round-trip'ов на клон, который не копирует ни байта), но на порядок меньше, чем «hundreds to thousands of round trips» плюс полные пересборки.

5) Достижимость и «одна транзакция на часть» — проверено, а не предположено. Триггеры из finding'а реальны:
- `FREEZE`/клон: `DataPartStorageOnDiskBase::freeze` при CAS-диске сам создаёт ОДНУ disk-транзакцию на всю часть, если внешней не передали — `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:530-540` («run the whole clone through ONE self-created disk transaction so all files land in a single content-addressed part»), далее `Backup(...)` дергает `disk->createHardLink` на файл (`:476`).
- `ALTER ... UPDATE`: `MutateTask` хардлинкует и переименования, и неизменённые файлы через `createHardLinkFrom` (`src/Storages/MergeTree/MutateTask.cpp:2206, 2464, 2500`), которое у `DataPartStorageOnDiskFull` идёт в `executeWriteOperation` → `disk.createHardLink` (`src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:355-369`), а транзакция на новой части открыта заранее (`MutateTask.cpp:3445`, `:3323`).
- Путь `BACKUP` через временные хардлинки на CAS-диске вообще закрыт `SUPPORT_IS_DISABLED` (`DataPartStorageOnDiskBase.cpp:421-427`), так что этот триггер к делу не относится.

Из того, что все вызовы идут через ОДНУ `ContentAddressedTransaction`, следует и величина потерь, и форма фикса.

6) **Фикс уже существует в этом же файле, для соседней операции.** `unlinkFile` мемоизирует доказательство на уровне `(транзакция, ref)` и понижает остаток пачки до `CachedForLoad`: `ContentAddressedTransaction.cpp:1595-1603` — «One mandatory body-HEAD per (transaction, ref), not per file … The first unlink re-proves the body ForceFresh; the rest of the burst reuses that proof», через `force_fresh_validated_refs`, который сбрасывается на закрытии транзакции (`:516-517`). Безопасность понижения не постулируется, а обеспечена кодом: `CachedForLoad`-хит отдаётся только если `cached->manifestId() == resolved->manifest_id` против свежего resolve, иначе view перестраивается (`Parts/PartFolderAccess.cpp:176-190`). Ровно тот же аргумент применим к committed-ветке `createHardLink`: источник — один и тот же ref, resolve остаётся свежим на каждом вызове, смена манифеста источника внутри транзакции будет замечена. Это НЕ изменение протокола (обязательный `HEAD` перед `PUT`/публикацией не трогается), а внутритранзакционная мемоизация — то есть под стоящее пользовательское вето не попадает.

7) Дефолт `always` менять не надо. Это fail-closed политика (`Parts/PartFolderAccess.h:240`, `Parts/PartFolderAccess.h:135-138`), а вопрос ослабления значения `never` уже отдельно отслеживается: `docs/superpowers/cas/BACKLOG.md:418` `[part-folder-validate-never-gating]` — там требуют гейт вместо молчаливого приёма `never`. Так что «default `always` = причина» формально верно, но лечить надо не дефолт.

**BACKLOG.** Существующего покрытия ровно этого класса нет. Ближайшие, но НЕ те же: `docs/superpowers/cas/BACKLOG/performance.md:141` `[manifest-trust-promote-path]` (пропуск per-leaf `HEAD`/`loadMeta` на promote-пути — другая операция), `:26` `[B121 / B202 / one-GET-open]` (сокращение GET-трафика чтения), `:144` `[ca-trycommit-retry-loses-staged-state]` (упоминает `createHardLink`, но про потерю staged-состояния при retry). Предшествующий ID `CAS-086` в BACKLOG не заведён — то есть перенос из прошлого раунда действительно не был оформлен. Добавлена новая секция в `docs/superpowers/cas/BACKLOG/performance.md`, анкер `{#hardlink-per-file-forcefresh-head}` (не закоммичена).

**Приоритет.** P2, не P1: это чисто PERF/SCALE, поведение fail-closed и корректное (никакой молчаливой порчи), деградация линейна по числу файлов части и проявляется на mutation/freeze, а не на горячем пути вставки/чтения. Фикс дешёвый и локальный, но требует прогонов (стоимость ошибки — ослабленное доказательство тела манифеста), поэтому после релиза, а не перед.

## CAS-056 — форма кода реальна — standalone-запись в закоммиченную часть действительно платит второй (черновой) manifest-PUT и по одному adopt-событию на перенесённый blob-лист, но «два ПОЛНЫХ manifest-энкода» и «внутри retry-замыкания CAS» — неточности; корректность не страдает. (частично, P2) {#cas-056}

Что подтвердилось на HEAD (якоря аудита `CA/...` устарели; файлы переехали коммитом 592b9b83568, номера строк сдвинуты):

1. Двойная публикация manifest-тела. В carry-forward-ветке `ContentAddressedTransaction::publishStaging`, если транзакция что-то стейджила (`st.build != nullptr`), сначала ставится и прекоммитится ЧЕРНОВОЙ манифест — `ContentAddressedTransaction.cpp:356-358` (`const Cas::ManifestId scratch_id = st.build->stageManifest(st.entries); st.build->precommitAdd(ns, ref, scratch_id); uploadPendingBlobs(st);`), затем `repointRef` публикует смерженный манифест — `ContentAddressedTransaction.cpp:380`. `repointRef` → `publishEntries` → `prepareEntries` стейджит ВТОРОЕ тело (`Parts/PartFolderAccess.cpp:484`), а `stageManifest` действительно делает durable `PUT` тела (`Pool/CasPartWriteTxn.cpp:854-881`, `stagingPutIfAbsent`). То есть два manifest-`PUT` на одну изменённую строку файла — подтверждено.

2. Черновое тело не удаляется писателем. Так как черновик был прекоммичен, `abandon` обязан не writer-delete'ить тело, а дописать точное precommit-removal, оставляя тело GC (`Pool/CasPartWriteTxn.cpp:1312-1318`); вызов `st.build->abandon()` — `ContentAddressedTransaction.cpp:387`. Итоговая цена одной standalone-записи/unlink по закоммиченной части: 2 manifest-`PUT`, ~4 append'а в ref-лог (scratch precommit, repoint precommit, promote, scratch removal), обязательный manifest-`GET` на promote (`Pool/CasPartWriteTxn.cpp:1035`) и одно удаление в GC. «Mutation version bump», который трогает каждую часть, умножает это на число частей — триггер аудита корректен.

3. По одному adopt-событию на blob-лист. Подтверждено: цикл ревалидации в замыкании promote идёт по ВСЕМ записям смерженного манифеста и для каждого tokenless-adopted blob-листа делает `ProfileEvents::increment(ProfileEvents::CASBlobAdoptTrusted)` + `EventEmitter{...}.emit(... BlobReuseAdopt ...)` — `Pool/CasPartWriteTxn.cpp:1123-1155`. Для свежей сборки записи tokened и пропускаются (`:1127-1128`), так что явление специфично именно для репойнтов: репойнт широкой части пишет по строке в `system.content_addressed_log` на каждый файл части. Смежно с `{#ca-log-tables-restart-cost}` (BACKLOG/gc.md:64).

Что неверно / преувеличено:
- «two full manifest encodes and PUTs»: черновой манифест собирается над `st.entries`, то есть только над ДЕЛЬТОЙ (изменённые/добавленные записи), а не над всей частью — это прямо задокументировано в `ContentAddressedTransaction.cpp:327-337` и видно по мержу на `:361-368`. Полным является только второй манифест. (Третий энкод есть, но чисто in-memory — проба на побайтовое равенство в `Parts/PartFolderAccess.cpp:559-565`, без `PUT`.)
- «per-entry adopt work executed inside the conditional-write retry closure»: `adoptEvidence` — чистая вставка в in-memory `deps`, без единого обращения к бэкенду (`Pool/CasPartWriteTxn.cpp:766-781`), и она вызывается ВНЕ замыкания, в `prepareEntries` (`Parts/PartFolderAccess.cpp:480-481`). Внутри замыкания находится только цикл ревалидации/эмиссии (п.3), а `build_ops` по контракту исполняется не более одного раза на элемент (`Pool/CasRefLedger.cpp:2891-2900`), при конфликте лог-транзакции лане не крутит замыкание, а падает громко (`Pool/CasRefLedger.cpp:3603-3657`). Значит O(entries) на публикацию, а не O(entries × попыток).
- «a no-op mutation costs two publishes»: на стороне `repointRef` есть короткое замыкание на побайтовое равенство с ZERO pool mutations (`Parts/PartFolderAccess.cpp:555-568`), так что второй публикации не происходит; черновой `PUT` + два append'а всё же остаются.

Достижимость: подтверждена, ветка живая — это нормальный путь для standalone-записей и removal-mark'ов по закоммиченной части (`Parts/PartFolderAccess.cpp:576-581` логирует именно этот класс). Класс дефекта — PERF/SCALE, не корректность: ни потери данных, ни тихой порчи; худшее — лишние объекты и события.

BACKLOG/история: класс «репойнт стоит дорого» частично покрыт — `BACKLOG/performance.md:55-58` (пункт (4), repoint waste на удалении части, ≈22% writer-`PUT`, статус DECISION NEEDED), `:60-63` (пункт (5), безусловный manifest-`GET` на promote), `BACKLOG/gc.md:42` (PART-REMOVAL-REPOINT), `BACKLOG/performance.md:141` (manifest-trust promote path). Ни один из них не описывает именно ЧЕРНОВОЙ второй manifest-`PUT` + его GC-мусор и не описывает O(entries) audit-строк на репойнт. Это и есть остаток; carry-forward-репойнт добавлен коммитом c08d89f2e35 (`git log -S "scratch_id"`).

Что осталось: добавил новую секцию `{#standalone-write-scratch-manifest-cost}` в `docs/superpowers/cas/BACKLOG/performance.md` (не закоммичено) с точной ценой, разбором неточностей аудита и направлением исправления: двухфазная ручка `prepareEntries` + `promote` уже существует, поэтому репойнт мог бы стейджить СМЕРЖЕННЫЙ манифест один раз, прекоммитить его, залить блобы под этой дугой и затем promote — одно тело вместо двух без ослабления EDGE-BEFORE-OBSERVE. Это трогает порядок протокольных шагов, поэтому требует такого же явного разрешения, как пункты (4)/(5) (стоящее вето на «дешёвые» протокольные оптимизации). Объём audit-строк лечится независимо (одна агрегированная строка класса `BlobReuseAdopt` на публикацию со счётчиком, per-leaf `ProfileEvent` оставить). P2 — отслеживать после релиза, блокером не является.

## CAS-057 — бросок `LOGICAL_ERROR` на не-стейдженный `moveFile`/`replaceFile` подтверждён и сохраняет ранее принятую позицию (fail-loud-заглушка без живого вызывающего), а «свежая улика» ложная: `DeleteBitmapFileOps::writeBitmapToStorage` не имеет ни одного продакшн-вызова. (not-a-bug, P3) {#cas-057}

Форма кода на HEAD (якоря аудита `CA/...` устарели, файлы переехали коммитом 592b9b83568):

1. Бросок подтверждён. `ContentAddressedTransaction::moveFile` обслуживает только источники, застейдженные В ЭТОЙ ЖЕ транзакции (`ContentAddressedTransaction.cpp:1504-1527`); иначе — `throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile source not staged: {}", path_from)` на `ContentAddressedTransaction.cpp:1534`, с комментарием на `:1529-1533`, который прямо называет эту ветку «no live caller ... retained only as a fail-loud guard for an unsupported mutation shape». `replaceFile` действительно только сбрасывает застейдженное состояние получателя и делегирует (`:1537-1552`). Так что «`.tmp` + `replaceFile` через ДВЕ транзакции по опубликованной части не работает» — верно.

2. Что при этом происходит по шагам. Имя файла битмапы — `delete_bitmap_<csn>.rbm` (`src/Storages/MergeTree/UniqueKey/DeleteBitmap.cpp:29-30`, `:512-515`), суффикс не blob-обязательный (`ContentAddressedTransaction.cpp:65-73`), поэтому autocommit-запись `.tmp` ДОПУСКАЕТСЯ гейтом `:766-771` и коммитится репойнтом, добавляя `<name>.tmp` в закоммиченный манифест; следующий `replaceFile` (своя транзакция, `DataPartStorageOnDiskFull::replaceFile` → `executeWriteOperation` → `disk.replaceFile`, `src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:336-343`) падает с `LOGICAL_ERROR`. Это громкий отказ, а не тихая порча: неверные байты никогда не публикуются, остаточный эффект — запись `<name>.tmp` в манифесте (снимается обычным `removeFileIfExists` → `unlinkFile`). По шкале аудита это самый низкий класс.

3. Названный вызывающий не является продакшн-путём — ключевая проверка достижимости, которую аудит не сделал. `DeleteBitmapFileOps::writeBitmapToStorage` (`src/Storages/MergeTree/UniqueKey/DeleteBitmapFileOps.cpp:47-71`: `storage.removeFileIfExists(tmp)`, `storage.writeFile(tmp)`, `storage.replaceFile(tmp, final)`) вызывается только из `MergeTreeBitmapStore::installBitmap` (`src/Storages/MergeTree/UniqueKey/MergeTreeBitmapStore.cpp:123`), а у `installBitmap` вообще нет вызывающих вне gtest'ов — grep по `src/` даёт только `UniqueKey/` и `UniqueKey/tests/`; собственный комментарий стора это фиксирует: «Acceptable now (installs serialized per partition, small/infrequent writes, no production caller)» (`MergeTreeBitmapStore.cpp:111-113`). Сама фича UNIQUE KEY экспериментальная и выключена по умолчанию (`src/Storages/MergeTree/registerStorageMergeTree.cpp:740-748`, `allow_experimental_unique_key`), путь установки битмап не подключён к вставке. Итог: «unique-key delete bitmaps fail on CAS with an internal error» на HEAD недостижимо, посылка для «re-open» из one-liner'а ложная.

4. Поддерживаемая форма для таких вызывающих уже есть, и она generic, не CA-специфичная: `IDataPartStorage::supportsAtomicFileWrites` (`src/Storages/MergeTree/IDataPartStorage.h:198-200`, для CA — `ContentAddressedMetadataStorage.h:261`), которую `VersionMetadataOnDisk::storeInfoToDataPartStorage` уже читает, чтобы писать `txn_version.txt` одним заходом вместо rename-танца (`src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp:329`; добавлено коммитом 45e43b37aaf, `git log -S "supportsAtomicFileWrites"`). Альтернатива — обе операции в ОДНОЙ `disk->createTransaction()`, где `.tmp` застейджен и `moveFile` перекладывает запись на месте (`ContentAddressedTransaction.cpp:1504-1527`). Именно на этот механизм и ссылается комментарий у броска.

Ранее принятая позиция (CAS-007, «Filimonov: should be fine — tests catch nothing; glance someday — not a blocker») коду на HEAD по-прежнему соответствует и не пересматривается: ветка документирована как fail-loud-заглушка, живого вызывающего нет, generic-механизм обхода существует и уже используется.

Пересечения с CAS-035 нет: CAS-035 — про полное перечисление `cas/ns/stream/` в GC-раунде и двойную материализацию edge-run'а (`Gc/CasBlobInDegree.cpp`, `Gc/CasGc.cpp`); CAS-057 — про интерфейс дисковой транзакции на пути MergeTree. Дубликатом не является.

BACKLOG: покрытия не нашлось — grep по `docs/superpowers/cas/BACKLOG.md` и `BACKLOG/*.md` по `unique key|bitmap|replaceFile|moveFile|supportsAtomicFileWrites` даёт только неродственный `[codex-26]` (`BACKLOG/performance.md:31`). Поэтому добавил (не закоммичено) новую секцию `{#tmp-replacefile-on-committed-part}` в `docs/superpowers/cas/BACKLOG/replication.md` (топик — MergeTree-интеграция) как latent-долг: когда путь UNIQUE KEY будут подключать, `writeBitmapToStorage` обязан либо взять короткое замыкание `supportsAtomicFileWrites`, либо обернуть оба шага в одну транзакцию, плюс тест на CA-диске. До тех пор поведение корректно (fail-closed), исправлять в коде CAS нечего — отсюда P3 и PRE-RELEASE: нет.

## CAS-040 — Механика находки реальна и достижима обычным DDL (проекция с `\n` в имени), но следствие описано неверно: коммита нечитаемой части нет (INSERT падает fail-closed, данные не теряются) — зато осиротевший манифест навсегда заклинивает каждый раунд GC во всём пуле. (подтверждено, P1) {#cas-040}

## Что подтверждается в коде на HEAD

Якоря находки указывают на старые пути `CA/Formats/...`; файл переехал в
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp`
(перенос подкаталогов — `592b9b83568`). Номера строк в находке устарели, актуальные:

- `bannerFor` строит баннер из СЫРОГО пути: `Formats/CasPartManifestFormat.cpp:68-71`
  (`return "==> " + String(path) + " il=" + std::to_string(n) + " <==";`) — в находке `:64-67`.
- Баннер добавляется в зону payload как есть, без экранирования: `:116-120` — в находке `:106-110`.
- Строка записи entry при этом путь ЭКРАНИРУЕТ (`writeStringValue` → `CasJsonWriter::stringValue`,
  «Quoted JSON string with full escaping», `Formats/CasTextFormat.h:75`): `:47`.
- На декоде баннер сравнивается с одной прочитанной строкой: `:278-282` — в находке `:248-252`.
- Валидация пути (относительный, без пустых/`.`/`..` сегментов) существует ТОЛЬКО на стороне декода:
  `:198-210` — в находке `:184-193`. Контрольные символы она не отвергает; на стороне encode
  проверки пути нет вообще (единственные проверки encode — сортировка и запрет дублей, `:77-87`).
  Grep по всему CA-дереву: единственные места с «invalid entry path» — `:201-208`, то есть
  асимметрия encode/decode подтверждается исчерпывающе.

Прямая проверка round-trip (сборка через `.claude/tools/cppexpr.sh` со связкой `dbms`, лог
`tmp/cas040_probe.log`):

```
probe("plain.txt")                 -> ROUNDTRIP_OK
probe("p\nq.proj/columns.txt")     -> DECODE_THREW: PartManifest: payload-zone banner mismatch, expected '==> p
probe("a/../b")                    -> DECODE_THREW: CAS part manifest: invalid entry path 'a/../b'
probe("weird il=3 <==")            -> ROUNDTRIP_OK
```

То есть «encode проходит, decode не может никогда» — верно, а вот баннер-подобный текст в имени
(`... il=3 <==`) безопасен, потому что баннер сверяется целиком.

## Достижимость: подтверждена, причём обычным пользовательским DDL

Путь entry берётся из `Route::file` (`ContentAddressedMetadataStorage.h:459-470`) — то есть буквально
из имени файла внутри каталога части; никакой гигиены имён на write-path нет. Имена столбцов и
индексов проходят через `escapeForFileName`, но имя каталога проекции — нет:
`src/Storages/ProjectionsDescription.h:156` — `String getDirectoryName() const { return name + ".proj"; }`.
Проверено на обычном локальном диске: `create table t (a UInt64, projection `p\nq` (select a order by a))`
создаёт каталог части `p<LF>q.proj` (вывод `cat -A`: `p$` / `q.proj$`).

Живая проверка на CA-диске (`clickhouse-local`, конфиг с `metadata_type=cas`, локальный бэкенд,
каталог `tmp/castest`):

```
Code: 246. DB::Exception: PartManifest: payload-zone banner mismatch, expected '==> p
q.proj/checksums.txt il=259 <==', got '==> p'. (CORRUPTED_DATA)
```

## Где находка ошибается (в свою пользу и себе во вред)

1. НЕВЕРНО: «a committed part permanently unreadable, with the corruption created by the writer».
   Манифест перечитывается внутри той же транзакции INSERT, поэтому INSERT падает fail-closed:
   `select count() from t` → `0`, `select count() from system.parts where table='t'` → `0`. Никакой
   закоммиченной нечитаемой части и потери данных нет; таблица просто становится непригодной для
   вставки (каждый следующий INSERT падает так же). Это громкий отказ, а не тихая порча.
2. НАЙДЕНО ХУЖЕ, чем в находке: неудавшаяся попытка оставляет в пуле недекодируемый объект манифеста
   (в `tmp/castest/cas_pool/ca/cas/manifests/...`), а sweep осиротевших манифестов декодирует каждый
   кандидат без защиты — `Gc/CasOrphanManifestSweep.cpp:878`
   (`const PartManifest body = decodePartManifest(...)`), и на месте вызова
   `Gc/CasGc.cpp:3124` (`result.orphan_sweep = planManifestCursorPage(...)`) нет никакого `try`.
   Исключение выходит из fold, курсор не двигается. Наблюдаемо в живом логе (`tmp/castest/ch.log`,
   `gc_interval_sec=3`), каждый тик:

   ```
   <Error> cas_pool/::ContentAddressedGC: CA GC round failed (will retry next tick):
   Code: 246. DB::Exception: PartManifest: payload-zone banner mismatch, expected '==> p
   q.proj/checksums.txt il=259 <==' ... (CORRUPTED_DATA)
   ```

   То есть одно странное имя проекции навсегда останавливает освобождение места во ВСЁМ пуле, пока
   оператор не удалит объект руками. Это и определяет приоритет: не потеря данных, а бессрочная
   остановка reclaim, инициируемая любым, кто может создать таблицу с проекцией.

## Что нашлось в BACKLOG и истории

- Прямого покрытия нет: grep по `docs/superpowers/cas/BACKLOG.md` и `BACKLOG/*.md` по
  `banner`/`bannerFor` — пусто.
- Ближайшие родственники (не покрывают этот случай):
  `BACKLOG/gc.md` {#ckpt-damage-no-repair-path} — недекодируемый `_ckpt` закрывает раунд-широкий
  destructive gate и не имеет пути ремонта; `BACKLOG.md` {#damaged-object-repair} — «present and
  undecodable» как класс для fsck. Оба явно про ПОВРЕЖДЕНИЕ извне (вне модели отказов доверенного
  стора), тогда как здесь недекодируемый объект пишет сам продукт из легального DDL.
  `BACKLOG/formats-and-storage.md` {#gc-state-encode-no-line-cap} — тот же класс асимметрии
  encode/decode, но для `gc/state` и с недостижимым следствием.
- `git log -S "bannerFor" -- src` → единственный коммит `3cae1327cbc` (формат v3, фаза 6): баннер
  такой с рождения, ничем позже не закрывался. Находка НЕ устаревшая.
- Пробел в тестах: `src/Disks/tests/gtest_cas_part_manifest_format.cpp:130`
  (`InlineBytesWithEmbeddedSpecialCharsRoundTripByteFaithfully`) намеренно фиксирует, что `\n` в
  ТЕЛЕ inline-файла безопасен, но тот же вопрос про ПУТЬ не задан ни разу;
  `DecodeRejectsMalformedEntryPaths` (`:218`) закрывает только traversal.

## Что реально осталось (внесено в BACKLOG некоммиченным)

Новая секция {#manifest-entry-path-newline-banner} в
`docs/superpowers/cas/BACKLOG/formats-and-storage.md`. Направление фикса:
(1) валидация пути на стороне ENCODE тем же правилом, что на декоде, плюс запрет управляющих
символов (минимум `\n`) — тогда громкий отказ случается ДО публикации объекта;
(2) sweep осиротевших манифестов не должен ронять раунд: недекодируемый манифест — записанная
аномалия + continue (форма как у `BodyUndecodable` для `_ckpt`), иначе один poison-объект
останавливает reclaim во всём пуле;
(3) экранирование имени каталога проекции — это generic MergeTree-код, требует upstream-консультации.

## CAS-041 — Механика (digest = канонический re-encode) на HEAD ровно как описано, но заявленное следствие `CORRUPTED_DATA` на чужом поле сегодня недостижимо (версионный гейт срабатывает раньше и громче); реальный остаток — политика `Tolerant` для этого формата мертва плюс измеренные 27-63% времени декода и две лишние копии payload. (частично, P2) {#cas-041}

## Что подтверждается в коде на HEAD

Якоря находки — старые пути `CA/Formats/...`; файл на HEAD:
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp`
(переезд подкаталогов — `592b9b83568`). Номера строк устарели, актуальные:

- Пересчёт и сравнение digest: `:297-301` (в находке `:263-267`):
  `const UInt128 expected_digest = computePayloadDigest(m); if (expected_digest != m.payload_digest) throw ... CORRUPTED_DATA`.
- `computePayloadDigest` делает глубокую копию модели и заново кодирует её: `:306-317`
  (в находке `:272-279`) — `PartManifest probe = m; probe.payload_digest = UInt128{}; const String bytes = encodePartManifest(probe);` + CityHash128.
- Декод действительно терпим к неизвестным ключам: `JsonObjectReader(..., KeyStrictness::Tolerant, ...)`
  на всех трёх уровнях (`:138` дескриптор, `:175` записи) и `r.skipUnknown(key)` (`:152`, `:184`, `:224`),
  а сам формат зарегистрирован как `Tolerant`: `Formats/CasFormat.cpp:165`.

То есть описание формы кода верно на 100%: терпимость к неизвестным ключам и digest-по-re-encode
взаимно несовместимы — пропущенный ключ не переживёт повторную сериализацию из локальной структуры.

## Где находка ошибается: следствие недостижимо

«Trigger: read a manifest written by a build with any additional field» не является достижимой
конфигурацией на HEAD:

- Писатель всегда штампует `G_BUILD`: `Formats/CasFormat.h:132-138` — «Until the roster and
  write-down-to-floor policy exist, this is always `G_BUILD`». Политики write-down нет (числится как
  B180, `BACKLOG/operability-and-introspection.md`).
- Значит объект из более новой генерации отвергается РАНЬШЕ digest'а — в заголовке:
  `Formats/CasTextFormat.cpp:320-328` (`expectHeaderLine` → `checkCompatibility(h.v, ...)`) →
  `UNKNOWN_FORMAT_VERSION`. Это происходит на `CasPartManifestFormat.cpp:129`, за ~170 строк до
  проверки digest на `:297`.
- Все реальные бампы генераций 4-9 — recreate-only (`Formats/CasFormat.h:24-59`), персистентных
  данных до релиза нет, compat-обязательств нет ([[feedback_ca_no_compat_scaffolding_predev]]).
- Даже в гипотетическом будущем это отказ fail-closed (громкий `CORRUPTED_DATA`), а не тихая порча.
  Формулировка сводки находки («silent additive-field loss became a hard fail») подаёт это как
  ухудшение, хотя жёсткий отказ строго лучше тихой потери поля; настоящая претензия не в этом, а в
  том, что аффорданс `Tolerant` для данного формата не работает никогда.

## Что реально осталось

1. Противоречие дизайна (не баг во времени исполнения): формат объявлен `Tolerant`
   (`Formats/CasFormat.cpp:165`), а фреймворк форматов прямо предусматривает аддитивные изменения,
   сохраняющие прежний reader floor (`Formats/CasFormat.h`, док к `FormatChangePoint`: «Additive
   changes retain the previous reader floor»). Для `cas_part_manifest` этим воспользоваться нельзя,
   пока digest — канонический re-encode. Решать это надо вместе с уже числящимся решением о
   CRC-границе `PartManifest.payload_digest` перед format-freeze:
   `BACKLOG/formats-and-storage.md` {#codecs-and-protocol} («decide the `RunRef.checksum` /
   `PartManifest.payload_digest` CRC-boundary before release»). Два выхода: считать digest по
   байтам провода (тогда терпимость становится настоящей и re-encode исчезает) либо перевести
   формат в `Strict` и честно записать, что аддитивная эволюция этого объекта требует бампа генерации.
2. Стоимость — реальна, но в находке преувеличена. Измерено на этой сборке
   (`tmp/cas041_bench.log`, `.claude/tools/cppexpr.sh`, 20 итераций на точку): доля
   `computePayloadDigest` в полном `decodePartManifest` — 27-63%:
   `60 × 100 B` → 45 µs, из них 12 µs (27%); `500 × 2 KiB` (1.06 MB) → 510 µs, из них 200 µs (39%);
   `200 × 64 KiB` (13.1 MB) → 4.6 ms, из них 2.9 ms (63%). То есть «2x work» — верхняя граница
   (~1.4x-2.7x суммарно), а не константа. Заявленные «~3x transient memory» держатся: одновременно
   живут декодированная модель, её глубокая копия (`probe`) и закодированные байты; при
   `object_cap` = 256 MiB для этого формата (`Formats/CasFormat.cpp:165`) пик стоит ограничить.
   Смежное: {#sec4-decoder-size-bounds} в том же файле (границы размера у декодеров).

## Дубликат/история

Не дубликат: OLD-CAS-027 (по `tmp/2031/gist/RECONCILIATION-2031.md:329`) — про тихую потерю
аддитивных полей; исправление OLD-CAS-025 (пересчёт digest) заменило её на жёсткий отказ. На HEAD
жёсткий отказ по чужому полю недостижим (см. выше), так что «эволюция» находки — про будущее, а не
про текущее поведение. Новая (2031-triage) секция CAS-027 в
`BACKLOG/docs-and-cleanup.md` {#pool-trust-boundary-undocumented} — про другое (граница доверия
пула), пересечения нет.

## Внесено в BACKLOG (некоммиченно)

Новая секция {#manifest-digest-by-reencode} в
`docs/superpowers/cas/BACKLOG/formats-and-storage.md` — фиксирует оба остатка (мёртвая политика
`Tolerant` + измеренная стоимость) со ссылкой на решение о CRC-границе в {#codecs-and-protocol}.

## CAS-058 — `freezeRemote` действительно единственный из трёх clone-путей без CAS-транзакции, кросс-дисковый `ATTACH PARTITION FROM` в CAS падает на первой же части — это уже подтверждённый и воспроизведённый на HEAD issue #2173 с запланированным пред-релизным фиксом; неверны только «REPLACE PARTITION FROM» в триггере и намёк на тихое «partial state» (отказ громкий, tmp-часть подчищается). (подтверждено, P1) {#cas-058}

Анкор находки (`src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:593-621`) устарел по номерам строк (снапшот до 592b9b83568 + последующие правки): на HEAD `freezeRemote` — `DataPartStorageOnDiskBase.cpp:615-668`, «правильные» ветки с транзакцией — `freeze` (`:542-544`, комментарий про B21) и `clonePart` (`:735-757`, комментарий «L2 (MOVE-to-CA fix)»), стример — `copyDirectoryContentIntoTransaction` (`:686-712`). Файл сам не переезжал (он в generic `src/Storages/MergeTree/`), переехали только CA-файлы.

Что подтверждается на HEAD:

1. `freezeRemote` не имеет CA-ветки. `DataPartStorageOnDiskBase.cpp:625-628`: транзакция берётся ИСКЛЮЧИТЕЛЬНО из `params.external_transaction`, иначе `dst_disk->createDirectories(to)`; ни одного `dst_disk->isContentAddressed()` в теле функции нет (единственные два вхождения `isContentAddressed` в файле рядом — `:542` в `freeze` и `:735` в `clonePart`). Для сравнения, `freeze` (`:540-544`) и `clonePart` (`:735-745`) сами создают `owned_transaction`/`clone_transaction`, а восстановление из бэкапа — `MergeTreeData.cpp:7543-7545` (`restore_tx`). То есть «третий путь забыли» — верно буквально.

2. Без транзакции копирование идёт по автокоммит-пути. `freezeRemote` вызывает `Backup(..., /*copy_instead_of_hardlinks=*/true, {}, params.external_transaction)` (`:630-643`); в `Backup` при пустой транзакции и `copy_instead_of_hardlinks=true` берётся ветка `src_disk->copyDirectoryContent(...)` (`src/Storages/MergeTree/Backup.cpp:180-185`), а это `IDisk::copyDirectoryContent` → `copyThroughBuffers` с пулом потоков (`src/Disks/IDisk.cpp:196-205`, `:174-193`) — каждый файл части становится отдельной автокоммит-транзакцией на CAS-диске.

3. Итог на CAS — громкий отказ. Для blob-обязательных файлов (`.bin`, `.mrk*`, `primary.idx` — `ContentAddressedTransaction.cpp:65-73`) автокоммит запрещён: `ContentAddressedTransaction.cpp:766-771` бросает `NOT_IMPLEMENTED` «Autocommit writes are not supported for content part files». Параллельно две автокоммит-транзакции на один и тот же ref дают вторую подпись — отказ unique-ref инварианта `Pool/CasPartWriteTxn.cpp:1168-1174` («promote: ref ... already names a different committed manifest», retry-later). Какая из двух сработает — гонка пула; обе — fail-closed.

4. Достижимость. `freezeRemote` вызывается только из `MergeTreeData::cloneAndLoadDataPart` (`MergeTreeData.cpp:9717`) в ветке `!on_same_disk`, а до неё стоит гейт `must_on_same_disk` (`MergeTreeData.cpp:9677-9681`, `BAD_ARGUMENTS` «disk does not belong to storage policy»). `must_on_same_disk=false` передают ровно ATTACH-ветки: `StorageMergeTree.cpp:3049-3057` и `StorageReplicatedMergeTree.cpp:9239-9248`; `external_transaction` там не задаётся (`ClonePartParams` — `StorageMergeTree.cpp:3029`, `StorageReplicatedMergeTree.cpp:9218-9222`). Значит триггер — именно `ALTER TABLE cas_tbl ATTACH PARTITION ... FROM src`.

Что в находке неверно:

- «`REPLACE PARTITION FROM`» и `MOVE PARTITION TO TABLE` этот путь НЕ достают. В Replicated `replace=true` жёстко передаёт `must_on_same_disk=true` (`StorageReplicatedMergeTree.cpp:9233`), как и `movePartitionToTable` (`:9521`). В `StorageMergeTree` `replace`/`movePartitionToTable` передают `!are_policies_partition_op_compatible` (`StorageMergeTree.cpp:3041`, `:3226`), а `StoragePolicy::isCompatibleForPartitionOps` требует, чтобы ВСЕ диски обеих политик были `isPlain()` (`src/Disks/StoragePolicy.cpp:420-435`); CAS-метаданные `isPlain()` не переопределяют, т.е. остаётся дефолтное `false` (`src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h:310`, `src/Disks/IDisk.h:473`) — политика с CAS-диском несовместима, `must_on_same_disk=true`, и запрос отвергается ЗАРАНЕЕ громким `BAD_ARGUMENTS`. Единственное исключение — одна и та же политика по имени (`StoragePolicy.cpp:422-423`), но тогда `on_same_disk` истинно и путь `freezeRemote` вообще не берётся.
- «leaving partial state rather than being rejected up front» — по факту не тихая порча. До броска успевают автокоммитом опубликоваться только inline-совместимые файлы части (`checksums.txt`/`columns.txt`/`count.txt`) в tmp-ref `tmp_replace_from_*`; ветка `Backup` с копированием обёрнута в `CleanupOnFail` → `dst_disk->removeRecursive(destination_path)` (`Backup.cpp:182-184`), плюс держится `getTemporaryPartDirectoryHolder(tmp_dst_part_name)` (`MergeTreeData.cpp:9687`). Целевая таблица не получает ни одной части (все `dst_parts` коммитятся только после успеха всех клонов). Так что реальный вред — падение запроса + возможный tmp-мусор в пуле, а не порча данных; «не отвергается заранее» — верно, но это UX/фича-гэп, а не корректность.

Покрытие в BACKLOG: уже есть, дословно про этот же дефект — `docs/superpowers/cas/BACKLOG.md:638` `## Issue #2173 CONFIRMED: cross-disk ATTACH PARTITION FROM (local -> CAS) — freezeRemote lacks the CAS single-transaction branch (2026-08-20) {#issue-2173-freezeremote-gap}`. Там же (`BACKLOG.md:646-663`) зафиксирован тот же механизм (16-потоковый пул, две конкурирующие автокоммит-транзакции, unique-ref refusal `CasPartWriteTxn.cpp:1168` либо `NOT_IMPLEMENTED` `ContentAddressedTransaction.cpp:770`), репро на HEAD и запланированный фикс — зеркалить CA-ветку `clonePart` внутри `freezeRemote` (self-created `dst_disk->createTransaction()` + `copyDirectoryContentIntoTransaction` + один commit) плюс stateless-тест. Пункт помечен как SCHEDULED pre-release (`docs/superpowers/cas/final-checks-todo.md`). На HEAD (`c2cd4b62df1`) фикс НЕ внесён — ветки в `freezeRemote` нет, так что запись не устарела. Новых записей в BACKLOG не добавлял: находка — независимое переоткрытие уже затрекованного и запланированного к фиксу issue #2173.

Итого: код-часть находки верна и это реальный пред-релизный дефект (первый же `ATTACH PARTITION FROM` в CAS-таблицу падает), но новизны нет; правки к формулировке — только ATTACH (не REPLACE/MOVE) и отказ громкий, без тихой порчи.

## CAS-059 — Все описания кода на HEAD верны (`DiskEncrypted` берёт любой делегат, не переопределяет `isContentAddressed`/`supportsAtomicFileWrites`, `use_fake_transaction` по умолчанию true), но CAS+шифрование — settled out-of-scope позиция (Filimonov), а связка отваливается громким `NOT_IMPLEMENTED` на первой же записи части; остаток — только отсутствующий fail-fast гейт в конфиге, P3. (by-design, P3) {#cas-059}

Предзаполненный вердикт one-liner'а (out-of-scope, «CAS+encryption needs design/dev/testing; should be workable later — not now») на HEAD по-прежнему соответствует коду: никакой поддержки шифрования в CAS нет, ни одного упоминания encryption/IV/key в `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, и позиция не переоткрывается. Проверял только, что позиция всё ещё описывает код, и что именно из «impact» реально достижимо.

Подтверждается на HEAD (анкоры находки по номерам строк совпали, файл `src/Disks/DiskEncrypted.*` не переезжал):

1. Нет проверки capability при создании. `src/Disks/DiskEncrypted.cpp:190-208` (`getDiskAndPathFromConfig`) берёт делегат из `DisksMap` по имени и валидирует ровно две вещи — непустое имя и завершающий `/` у `path`; фабрика `registerDiskEncrypted` (`src/Disks/DiskEncrypted.cpp:517-531`) конструирует `DiskEncrypted` и вызывает `startup(skip_access_check)`, без единого вопроса о типе делегата. То есть `<disk><type>encrypted</type><disk>cas_disk</disk></disk>` принимается.

2. Обёртка отвечает «не content-addressed». `src/Disks/DiskEncrypted.h` делегирует десятки предикатов, включая `isPlain` (`:332`), но `isContentAddressed`/`supportsAtomicFileWrites` в файле отсутствуют вовсе (grep по `DiskEncrypted.h`), значит работают дефолты `IDisk` — `false` (`src/Disks/IDisk.h:473-480`), тогда как CA-метахранилище отвечает `true` (`.../ContentAddressed/ContentAddressedMetadataStorage.h:261`, через `DiskObjectStorage::supportsAtomicFileWrites`, `DiskObjectStorage.cpp:779-782`). Следствие «все CA-хуки в MergeTree выключены» — верно: их условия читают именно `isContentAddressed()` (`DataPartStorageOnDiskBase.cpp:422`, `:542`, `:735`, `MergeTreeData.cpp:5937`, `:7544`, `IMergeTreeDataPart.cpp:1364`, `MergeTask.cpp:567`, `DataPartsExchange.cpp:161`, `:405`).

3. `use_fake_transaction` по умолчанию `true`: `src/Disks/DiskEncrypted.cpp:329` (`config.getBool(config_prefix + ".use_fake_transaction", true)`) и безусловно `true` во втором конструкторе (`:341`); `createTransaction` при этом возвращает `FakeDiskTransaction` (`src/Disks/DiskEncrypted.h:344-349`), т.е. каждая операция уходит в делегат как самостоятельный автокоммит.

4. Префикс пути существует: `DiskEncryptedTransaction::wrappedPath` (`src/Disks/DiskEncryptedTransaction.h:36-42`) приклеивает `disk_path` спереди; метахранилище тоже оборачивается `MetadataStorageWithPathWrapper` (`DiskEncrypted.h:376-384`).

Что в «impact» неверно или недостижимо:

- «fails only at the first part write» — верно, и это ГРОМКИЙ fail-closed, не тихая порча. Поскольку транзакции фиктивные, каждый файл части приходит в CA как автокоммит-запись, а для blob-обязательных файлов (`.bin`, `.mrk*`, `primary.idx` — `ContentAddressedTransaction.cpp:65-73`) автокоммит запрещён броском `NOT_IMPLEMENTED` (`ContentAddressedTransaction.cpp:766-771`). Любая реальная часть (и Wide, и Compact) содержит такой файл, так что INSERT падает всегда; конфигурации, в которой запись «тихо» проходит, нет. Стартовый write-probe (`IDisk::startup`/`checkAccess`) — это не part-файл, он ложится как обычный mountpoint-объект (`ContentAddressedTransaction.cpp:808-830`), поэтому сервер и правда поднимается — то есть «принимается на старте» подтверждается.
- «the wrapper's `path` prefix silently reshapes CAS path classification (shadow detection, atomic-shard detection, table-file parsing)» — верно лишь на треть, и только при непустом `<path>` (он может быть пустым: `DiskEncrypted.cpp:205-207` просто требует завершающий `/`, если строка непуста). Классификация part-путей к префиксу УСТОЙЧИВА: `findTableUuidComponent` ищет пару `<3 hex>/<uuid>` по форме в любом месте пути (`Parts/PartPathParser.cpp:114-128`, комментарий «robust to a missing store/»), а для не-Atomic раскладки есть форменный fallback по part-dir грамматике (`:136+`). Ломается только shadow-детекция, которая требует буквального первого компонента `shadow` (`Parts/PartPathParser.cpp:277-282`) — но FREEZE до этого всё равно не доживёт, потому что дойти до shadow-части можно лишь имея записанную часть, а запись падает (п. выше). Так что «silently reshapes» как отдельный вред — недостижимое следствие верно описанной формы кода (типичная ошибка этого аудита).
- Про «no capability check» как таковое — верно, и это единственный живой остаток.

История/покрытие: в `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` про шифрование была ровно одна строка — `BACKLOG/operability-and-introspection.md:25` `[B17] encryption-at-rest × content-addressing — DESIRABLE — Dedup scope per-encryption-key` (секция {#operability}); она покрывает ФИЧУ (и заодно per-key дедуп/крипто-шред, ср. вердикт CAS-028), но не отсутствующий гейт. Отсылка one-liner'а «the missing gate itself stays open under prev CAS-113» указывает на security-находку прошлого раунда, которая в этом раунде идёт как CAS-090 (`tmp/2031/issue-body.md:142`, SSE-C/manifest-plaintext/re-keying) — там про гейт тоже нет записи в BACKLOG. Признал остаток нетрекнутым и добавил (uncommitted) новую секцию `docs/superpowers/cas/BACKLOG/mounts-and-lifecycle.md:129` — `{#encrypted-over-cas-missing-gate}`: отказывать при конструировании/валидации конфига, если делегат (транзитивно) отвечает `isContentAddressed()`, с сообщением про неподдерживаемую связку; опционально — прокинуть `isContentAddressed`/`supportsAtomicFileWrites` через `DiskEncrypted`, когда связку начнут проектировать.

Итого: описание кода верное, вердикт out-of-scope на HEAD по-прежнему корректен, релизного блокера нет (отказ громкий и немедленный), остаток — дешёвый fail-fast гейт, P3.

## CAS-060 — Форма кода верна (случайный IV на каждую перезапись + CAS хеширует то, что ему дали → дедупа нет вовсе), но CAS×шифрование — принятая out-of-scope позиция; тихой порчи нет, а сама связка вообще не проведена (`DiskEncrypted` не пробрасывает `isContentAddressed`). (by-design, P3) {#cas-060}

**Что подтверждается на HEAD (обе технические половины находки — верны).**

1. Случайный IV на каждую rewrite-запись: `src/Disks/DiskEncryptedTransaction.cpp:105-112` (анкер находки точен и не устарел):
   ```
   if (!old_file_size)
   {
       /// Rewrite mode: we generate a new header.
       ...
       header.init_vector = FileEncryption::InitVector::random();
   }
   ```
   Заголовок с IV — часть тела файла (`src/IO/FileEncryptionCommon.h:136` — `InitVector init_vector;` внутри `Header`, `kSize = 64` — `:139`), т.е. байты файла различаются даже при идентичном plaintext. Append-режим (`:98-103`) переиспользует существующий header — но для CAS это неважно, в CAS каждая часть пишется как новый файл.

2. CAS хеширует ровно те байты, которые ему передали. Анкер находки `CA/ContentAddressedTransaction.cpp:600-642` **устарел** (снапшот до реорганизации `592b9b83568`); на HEAD хеширующий буфер ставится в `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1814` и `:1848` (`hashing = Cas::makeBlobHashingWriteBuffer(hash_algo, *sink);`), а имя блоба берётся из `:1876` (`const std::string hash_hex = hashing->getHashHex();`). Хеш — потоковый passthrough над уже зашифрованным потоком (`Primitives/CasBlobHashingWriteBuffer.h:17-28`), никакой нормализации/plaintext-хеша нет.

Следствие «дедуп исчезает» верно и даже сильнее, чем в находке: не только два реплики или ATTACH, а **любая** повторная запись байт-идентичного plaintext (даже тем же сервером, тем же ключом) даёт другой блоб. Счётчика/предупреждения об этом действительно нет — но и не может быть: CAS видит просто разные байты, для него это нормальный miss.

**Чего в находке нет и что снижает её вес.**

- Это не порча и не потеря данных, а регрессия по месту/стоимости. Дедуп — оптимизация; корректность чтения/записи не затрагивается. По принятой градации «fail-closed loud vs silent corruption» это даже не отказ, а отсутствие выигрыша.
- Связка «шифрование над CAS» на HEAD вообще не проведена, и это важнее самого IV: `DiskEncrypted` не переопределяет `isContentAddressed` (база возвращает `false` — `src/Disks/IDisk.h:477`; проброс есть только у `src/Disks/ReadOnlyDiskWrapper.h:92`). Поэтому под обёрткой все CA-ветки MergeTree видят НЕ-CA диск: `DataPartStorageOnDiskBase.cpp:422`, `:542`, `:735`, relink-путь `DataPartsExchange.cpp:161`, правило родительской транзакции для проекций `IMergeTreeDataPart.cpp:1364` и `MergeTask.cpp:567`, whole-part транзакция при BACKUP-restore `MergeTreeData.cpp:7544`. При этом сам `DiskObjectStorage` под обёрткой остаётся content-addressed (`DiskObjectStorage.cpp:774-776`), т.е. per-file autocommit-отказы CAS никуда не деваются — конфигурация упадёт громко, а не тихо испортит данные. Ни в фабрике дисков, ни в валидации маунта комбинация при этом не запрещена — тоже часть того же пробела.
- Тестов на «encrypted over CA» в дереве нет (в `tests/integration/test_disk_configuration/test.py:573` есть только encrypted-диск сам по себе), что подтверждает статус «не поддержано».

**История/BACKLOG.** Позиция settled и зафиксирована в one-liner'е (Filimonov: «CAS+encryption needs design/dev/testing; should be workable later — not now»), не пересматриваю. Уже затрекано как `docs/superpowers/cas/BACKLOG/operability-and-introspection.md:25` — «**[B17] encryption-at-rest × content-addressing** — DESIRABLE — Dedup scope per-encryption-key; local to key/hash derivation» (анкер секции `{#operability}`). Прошлый раунд триажа тот же вывод уже делал по соседней находке: `docs/superpowers/cas/2031-triage.md:1513` («B17 … Это ровно “half” находки про соль/шред … Отслежено, не сделано»).

**Реальный остаток (untracked → добавлен).** Сам per-key дедуп-скоуп затрекан, но два конкретных факта, которые обязана учесть будущая работа по шифрованию, нигде не записаны: (а) при случайном IV дедуп исчезает ПОЛНОСТЬЮ, а не сужается до per-key (значит per-key scope без детерминированного IV/plaintext-хеша ничего не даёт), и (б) `DiskEncrypted` не CA-прозрачен, поэтому обёртка обходит все CA-ветки и CA-отказы. Добавил секцию (не закоммичено): `docs/superpowers/cas/BACKLOG/operability-and-introspection.md` {#encrypted-wrapper-hides-content-addressed}.

**Итог.** Описание кода верно, следствие «дедуп молча теряется» верно, но приоритет низкий: это не дефект существующей функциональности, а свойство пока не поддержанной комбинации, закрытой принятым решением об out-of-scope. Ничего не надо чинить до релиза; P3 — только за счёт добавленной записи в BACKLOG.

## CAS-061 — Ядро подтверждено (единственный rebuild-верб — `gc/state`; каталог/`_ckpt` падают fail-closed без пути восстановления; все CA-инструменты открываются через `_pool_meta`), но это уже затрекано как {#damaged-object-repair}/{#ckpt-damage-no-repair-path}, часть про mount lease неверна (есть `cas-drop-member`), а отсутствие migration-тулинга — сознательное pre-release решение. (частично, P2) {#cas-061}

**1. «Только `gc/state` имеет rebuild-путь» — подтверждено.**
`programs/disks/CommandCaGcRebuild.cpp:29-60` — единственный верб пересборки (`cas-gc-rebuild`, `rebuildBaseline`), плюс его SQL-двойник `SYSTEM CAS GC REBUILD` (`src/Interpreters/InterpreterSystemQuery.cpp:1033-1035`, `src/Parsers/ParserSystemQuery.cpp:477-479`). Остальные CA-вербы — `cas-fsck` (`programs/disks/CommandFsck.cpp:24`), `cas-inspect` (`CommandCaInspect.cpp:28`), `cas-gc-dryrun` (`CommandCaGcDryRun.cpp:22`) — только читают; `cas-drop-member` (`CommandCaDropMember.cpp:26`) удаляет объекты мёртвого участника, но ничего не пересобирает. Никакого repair для каталога/`_ckpt`/fold seal нет.

**2. Каталог рефов: fail-closed без пути восстановления — подтверждено.** Анкер `CasRefCatalog.cpp:44-49` на HEAD соответствует `Pool/CasRefCatalog.cpp:41-49`:
```
if (!snapshot.token)
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "Mandatory CAS ref catalog '{}' is absent -- refusing to interpret opaque life "
        "objects as an empty ownership universe", ...);
```
`initializeEmptyForNewPool` (`:52-73`) — только bootstrap нового пула (`putIfAbsent` + требование канонически пустого тела), не DR. Отказ громкий, тихой порчи нет.

**3. Формат-floor и «no in-place migration» — подтверждено дословно.** `Formats/CasPoolMetaFormat.cpp:111-117`: при `header.v < kCommittedRefFrontierGeneration` — `UNKNOWN_FORMAT_VERSION` с текстом «…recreate the pool. … CAS is pre-release: there is no in-place migration.» Анкер `:89-95` устарел (реорганизация `592b9b83568`), сообщение живёт на `:112-117`. Но «migration tooling не существует» — это **by design** для pre-release (в BACKLOG это отдельный запланированный GATE: `BACKLOG/operability-and-introspection.md:19` «[B180 / format-freeze] … freeze the format on the first persisted-data release» и `:22` «[B13] migration path for existing tables»). То есть эта часть находки — не дефект, а известный незакрытый GATE.

**4. Chicken-and-egg через `_pool_meta` — подтверждено, и это самая ценная часть находки.** Все пять инструментов достают пул только через `ca->store()`, т.е. через `Cas::Pool::open`, которая заканчивается на `PoolMeta::createOrValidate(..., allow_mint=!config.read_only)` (`Pool/CasPool.cpp:494-496`). Все инструменты ОБЯЗАНЫ открывать диск read-only (`CommandFsck.cpp:51-54`, `CommandCaInspect.cpp:48`, `CommandCaGcRebuild.cpp:54`, `CommandCaGcDryRun.cpp:38`, `CommandCaDropMember.cpp:47`), а read-only-открытие минтить не имеет права — отсутствующий `_pool_meta` даёт `INVALID_STATE` (`Pool/CasPoolMeta.cpp:143-146`), недекодируемый — исключение из `decodePoolMeta`. Особенно показателен `cas-inspect`: пул ему нужен ровно для `backend().get(key)` и `layout()` (`CommandCaInspect.cpp:52-56`), т.е. метаданные пула для его работы не нужны вовсе, но без них он не запускается. Анкер находки `CA/Pool/CasPool.cpp:293-368` устарел — на HEAD это `Pool::open` на `Pool/CasPool.cpp:367-525`, а указанный «:351-353» соответствует нынешнему блоку минта/валидации `:488-496`.

**5. Что в находке неверно или преувеличено.**
- «damage to … a mount lease … has no recovery verb» — неверно: верб есть, это `cas-drop-member` — он «erase its namespaces, debris, staging, roots objects and **mount slot**» (`CommandCaDropMember.cpp:26-31`, `Tools/CasDecommission.h`). Недекодируемый lease при этом обрабатывается адресно и громко, а не «молча»: `Pool/CasServerRoot.cpp:725-729` («An undecodable lease is the WORST case for a recreation, not an ignorable one»), `:836-840` («undecodable -- fail closed, never wave through»).
- «Trigger: any corruption or **partial write** of a control object» — частично мимо: на S3-бэкенде PUT контрольного объекта атомарен, частичной записи нет; риск частичного файла ранее был локализован только для Emulated/local-бэкенда (см. `BACKLOG/operability-and-introspection.md`{#disk-error-audit-followups-2026-07-21}). Байтовая порча durable-объекта прямо объявлена вне доверительной модели (`BACKLOG.md`{#damaged-object-repair}: «outside the trusted-store fault model this design assumes, so this is not a correctness defect; it is an OPERABILITY hole»).
- Класс отказа — **fail-closed loud** во всех проверенных точках: `_pool_meta` present-but-undecodable на remount даёт `StayTransient` + `CASRemountHeldTransient` + WARNING с прямым текстом «the pool metadata is unreadable to this build» (`Pool/CasPool.cpp:1064-1078`), lifecycle-гейт классифицирует это отдельно (`Pool/CasPool.cpp:105-143`). Тихой порчи нет ни в одной из ветвей.

**6. Что уже затрекано (не новая находка).**
- `docs/superpowers/cas/BACKLOG.md:104` — **`[damaged-object-diagnose-and-repair]` {#damaged-object-repair}**: ровно эта тема, с разбивкой на 4 пункта — fsck должен различать *present-and-undecodable / absent / decodable-but-inconsistent* «per affected object kind (`_ckpt`, fold seal, `gc/state`, catalog), naming the exact key»; `ca-fsck --repair` должен пересобирать выводимое и громко отказывать для невыводимого; выход из `NeedsRecovery`; операторский runbook «a CAS object is damaged».
- `docs/superpowers/cas/BACKLOG/gc.md:65` — **{#ckpt-damage-no-repair-path}**: «a damaged `_ckpt` has no repair path and still shuts the round-wide destructive gate», с явным «there is NO repair path … the operator action for a damaged `_ckpt` is manual object surgery».
- `BACKLOG/gc.md:52` (`[gc-rebuild follow-ups]`) и `:142` (`[gc-rebuild-lease-interlock]`) — остатки по единственному существующему rebuild-вербу.
- Ни один из этих пунктов на HEAD не закрыт (repair-верба в `programs/disks/` нет; `CommandFsck.cpp` строк про present-and-undecodable не содержит).

**7. Реальный незатрекованный остаток (добавлен).** Список объектов в {#damaged-object-repair} не включает `_pool_meta` и не фиксирует само bootstrap-зацепление «инструменты открываются через повреждённый объект». Это ровно то, что в находке ново. Добавил секцию (не закоммичено): `docs/superpowers/cas/BACKLOG/operability-and-introspection.md` {#pool-meta-bootstrap-blocks-dr-tools} — с тремя пунктами долга: (а) pool-meta-less «raw backend + layout» открытие для `cas-inspect`/diagnose-only fsck; (б) fsck-строка present-and-undecodable для `_pool_meta`; (в) решение «`_pool_meta` восстанавливается или только реставрируется» — `pool_id` это случайный u128, минтящийся при создании, т.е. невыводим, значит честный ответ «restore, not repair», и он должен попасть в runbook.

**Итог.** `частично`: технические утверждения по коду верны (с поправкой на устаревшие анкеры), но следствие — операбилити-дыра с громкими отказами, а не риск данных; основная масса уже затрекана двумя существующими пунктами; часть про mount lease фактически неверна; migration — сознательный pre-release GATE. P2: чинить до релиза не требуется, но остаток (`_pool_meta` как единая точка отказа всех DR-инструментов) заслуживает трекинга.

## CAS-064 — Отсутствие фаззера и property-based тестов подтверждено фактически, но это settled-позиция (доверяем S3, декодеры fail-closed); реальный остаток — три живых формат-класса не зарегистрированы в общей battery (P3, дрейф-риск, а не дыра в покрытии). (by-design, P3) {#cas-064}

**Предварительный вердикт one-liner'а — settled design position** («Filimonov: same trust model as CAS-005 — trust S3; less trust ⇒ more perf loss; no decoder fuzz mandate as gate»). Не пересматриваю; проверил только, что позиция всё ещё соответствует коду. Соответствует: весь ввод декодеров приходит из префикса, которым CAS владеет монопольно, каждый декодер fail-closed, и это зафиксировано тестами (см. ниже).

**Факт-чек утверждений находки (все — про отсутствие):**

1. «absence of `CA/fuzzers/`» — ПОДТВЕРЖДЕНО. Каталога `src/Disks/fuzzers` не существует (fuzz-таргеты есть в `src/AggregateFunctions/fuzzers`, `src/Compression/fuzzers`, `src/Core/fuzzers`, `src/DataTypes/fuzzers` и т.д., в `src/Disks` — нет). Путь `CA/fuzzers/` из анкера — старый снапшот; в дереве после `592b9b83568` соответствующего места нет вообще.
2. «`ci/workflows/nightly_fuzzers.py` и `tests/fuzz/` не несут CAS-таргета» — ПОДТВЕРЖДЕНО: grep по `cas|content_addressed` в `ci/workflows/nightly_fuzzers.py` пуст; в `tests/fuzz/` нет ни одного CAS-словаря/опций.
3. «absence of `rapidcheck`/`RC_GTEST` under `src`» — ПОДТВЕРЖДЕНО: grep по `rapidcheck|RC_GTEST` по `src/` (cpp/h/txt) пуст. Property-based тестов в проекте нет вообще — это свойство всего репозитория, а не CAS.
4. «`src/Disks/tests/cas_format_test_battery.h` registrations omit `RunFile`, `RefCkpt`, `GcMaintenanceState`» — ПОДТВЕРЖДЕНО по существу, но с двумя важными поправками:
   - анкер указывает не на то место: регистрации живут не в `cas_format_test_battery.h` (там только сам движок — `runFormatBattery`, `src/Disks/tests/cas_format_test_battery.h:56`), а по одной в каждом `gtest_cas_*_format.cpp`;
   - живых `FormatId` — 18 (`.../ContentAddressed/Formats/CasFormat.h:96-129`); `Roster = 9` не в счёт, он зарезервирован и `traitsFor(FormatId::Roster)` бросает `LOGICAL_ERROR` (`CasFormat.h:192`, пин — `src/Disks/tests/gtest_cas_text_format.cpp:74,97`). Зарегистрированы через `runFormatBattery`: `GcState`, `GcHeartbeat` (`gtest_cas_gc_state_format.cpp:23,34`), `PartManifest` (`gtest_cas_part_manifest_format.cpp:73`), `FoldSeal` (`gtest_cas_fold_seal_format.cpp:41`), `PoolMeta` (`gtest_cas_format_battery.cpp:17`), `GcOutcomes` (`gtest_cas_gc_outcomes_format.cpp:39`), `RefSnapshot` (`gtest_cas_ref_snapshot_format.cpp:427`), `Owner`/`ServerEpoch`/`MountLease` (`gtest_cas_server_root_format.cpp:22,43,53`), `Blob` (`gtest_cas_blob_envelope_format.cpp:168`), `RefLog` (`gtest_cas_ref_log_format.cpp:783` + `gtest_cas_ref_epoch_seal_format.cpp:484`), `RefCatalog` (`gtest_cas_ref_catalog.cpp:219`), `BlobMeta` (`gtest_cas_blob_meta_format.cpp:15`) — 14 из 18. Не зарегистрированы ровно те три, что названы: `RunFile` (`cas_run`, `CasFormat.cpp:166`), `RefCkpt` (`cas_ref_ckpt`, `CasFormat.cpp:152`), `GcMaintenanceState` (`CasFormat.cpp:164`).

**Где находка переоценивает следствие (её обычная привычка):** «three live format classes skip the shared failure-mode battery» читается как «у них нет покрытия отказов». Это неверно. У каждого из трёх есть собственные, довольно плотные fail-closed тесты:
- `RunFile`: `src/Disks/tests/gtest_cas_record_stream_format.cpp:167` (seal checksum mismatch), `:194` (trailer count mismatch → `CORRUPTED_DATA`), `:206` (обрезка на границе строки), `:215-252` (header-гейты, `v+1` → `UNKNOWN_FORMAT_VERSION`, мусор → `CORRUPTED_DATA`);
- `RefCkpt`: `src/Disks/tests/gtest_cas_ref_ckpt.cpp:310,322,328,338,350,353,362-375` (malformed, unknown key, critical-key → `UNKNOWN_FORMAT_VERSION`, дубликат, header-only, обрезка последнего байта, половинчатые пары, трейлинг-мусор);
- `GcMaintenanceState`: `src/Disks/tests/gtest_cas_gc_maintenance_state_format.cpp:62-85,177` (несколько `CORRUPTED_DATA` + `v+1`).
Так что фактический дефект — не дыра, а риск дрейфа: новая «рука» battery (новое смещение обрезки, новое правило wrong-type) доедет до 14 классов и молча минует три. Это и есть остаток, который я внёс в BACKLOG.

**Про «impact»-строку находки** («every CAS decoder consumes bucket-sourced input that any credential holder can shape»): shape-уровень отказов у декодеров именно fail-closed и это утверждено самим движком battery — `cas_format_test_battery.h:70-104` требует `CORRUPTED_DATA` на каждой обрезке (по границам строк и внутри первой строки), `UNKNOWN_FORMAT_VERSION` на `v+1`, `CORRUPTED_DATA` на подмену типа валидным чужим типом и на ведущий мусор. То есть класс багов, о котором говорит находка (CAS-036..039), — это громкое исключение, а не тихая порча; по градации задания это низкий приоритет. Утверждение «no invariant (in-degree, exclusivity, lease safety) is checked by generated input» верно буквально, но тривиально (генерируемого ввода в проекте нет вовсе), а сами инварианты покрыты TLA+-моделями (`docs/superpowers/models/`) и gtest-батареей, то есть «не проверены фаззером» ≠ «не проверены».

**BACKLOG / история:** grep по `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` по `fuzz|property-based|rapidcheck` — ни одного попадания; тема фаззинга в публичном наборе документов CAS не заведена (единственные упоминания — в архивных `docs/superpowers/cas/random/fable-revew-20250805.md:120,192`, «no perf/stress/fuzzer/upgrade lane runs CAS-default storage»), что согласуется с тем, что мандат на фаззинг сознательно не брали. Отдельного закрывающего коммита здесь нет и быть не может — находка про отсутствие.

**Реальный остаток (внесён, не закоммичен):** новая секция и пункт `{#format-battery-three-classes-unregistered}` в `docs/superpowers/cas/BACKLOG/testing-and-ci.md` — зарегистрировать `RunFile`/`RefCkpt`/`GcMaintenanceState` в `runFormatBattery`, сохранив их семантические тесты; `Roster` не регистрировать (зарезервирован). Механическая работа, P3, релиз не блокирует.

## CAS-065 — Центральное утверждение ложно — нативный `If-None-Match`/`If-Match` путь гоняется в шести CI-полосах CAS-over-S3 (RustFS, `Mode::Native`) и на каждом writable-открытии пула проходит fail-closed capability-battery; реально не хватает только повторяемой полосы для GCS-generation-диалекта и native-строки в contract-suite, и это уже заведено. (частично, P2) {#cas-065}

**Что в находке верно (анкеры проверены на HEAD):**
- `src/Disks/tests/gtest_cas_backend_contract.cpp:250-258` — номера строк точны: параметризованный contract-suite инстанцируется ровно двумя рукавами, `INSTANTIATE_TEST_SUITE_P(CASInMemory, ...)` (`:250`) и `INSTANTIATE_TEST_SUITE_P(CASLocal, ...)` (`:253`), причём `CASLocal` — это `ObjectStorageBackend` над локальным object storage именно в `ObjectStorageBackend::Mode::EmulatedSingleProcess` (`:255-257`). Значит на уровне gtest contract-suite действительно нет строки `Mode::Native` над реальным проводом. Это верно.
- `src/Disks/tests/gtest_cas_backend_generation.cpp` — GCS-generation-диалект покрыт только в процессе: часть тестов форсирует диалект сеттером (`setNativeTokenTypeForTest(TokenType::Generation)`, `:100,118,145,156,182,218`), часть — фейковым S3-клиентом `FakeGenerationS3Client` (`:248-334`), поверх которого идут реальные проверки поведения, а не только настроек (`:373` cap → `NOT_IMPLEMENTED` до любого PUT, `:390` один PUT + generation из ответа, `:415`/`:428` отсутствующая и нечисловая generation → исключение). Живого GCS в CI нет — верно.
- `ci/defs/altinity_jobs.py:116-121` — «CAS over local object storage» действительно одна несанитайзерная полоса. Верно.

**Что в находке ложно — приписанное следствие:** «the entire native `If-None-Match`/`If-Match` path and the GCS generation-token path ship unverified end to end» и «the exclusivity guarantee is only ever tested against the emulated in-process backend».
1. Режим выбирается по типу object storage, а не по тестовой конфигурации: `ContentAddressedMetadataStorage.cpp:689-692` — `Mode::EmulatedSingleProcess` ровно и только для `ObjectStorageType::Local`, всё остальное (S3/GCS/совместимые) — `Mode::Native`. Значит любая CAS-over-S3 конфигурация в CI идёт нативным путём.
2. Таких полос в CI шесть: `ci/defs/altinity_jobs.py:73-115` — `amd_binary, cas s3 storage`, `amd_asan_ubsan` (шардированная), `amd_tsan` (шардированная), `amd_msan` (шардированная), `arm_binary`, поверх RustFS, причём комментарий `:75-76` прямо фиксирует, зачем RustFS, а не MinIO OSS: «the incarnation pool needs enforced conditional deletes». Плюс интеграционные сюиты на RustFS: `tests/integration/test_cas_gc_s3`, `test_cas_shared_pool`, `test_cas_replicated_relink`, `test_cas_ref_snaplog`, `test_cas_gc_sharded`, `test_cas_lazy_load_recovery`, `test_cas_insert_fault_recovery`, `test_cas_file_cache`, `test_cas_drop_pool_member` (grep `with_rustfs`).
3. И главное: нативная условная семантика не «предполагается», а доказывается на каждом writable-открытии пула. `Pool::open` (`.../Pool/CasPool.cpp:367-395`) прогоняет мутирующую capability-battery под `_probe/`, и она fail-closed: `.../Backend/CasProbe.cpp` бросает `NOT_IMPLEMENTED` на каждом непройденном шаге (`:61,67,75,80,103,108,118,121,125,136,144,153,161,168`), сравнивая заведомо неверные токены В ЖИВОМ диалекте (`:87-99`, `:150`) именно чтобы локальный guard диалекта не сделал проверку вакуумной. То есть если бы RustFS не enforce'ил `If-None-Match`/`If-Match`/условное удаление, все шесть полос падали бы на старте сервера, а не «проходили с эмуляцией». Экслюзивность нативного пути проверяется end-to-end на каждом запуске этих полос.
4. GCS-путь на живом сторе тоже не «неверифицирован»: `docs/superpowers/cas/BACKLOG/formats-and-storage.md:22` — «[GATE #1: Azure] real-store GC validation on Azure — GATE — AWS + GCS DONE (2026-07-03, live-validated). Azure not started». Это же зафиксировано в предзаполненном вердикте one-liner'а (prev CAS-012: «e2e tested on real S3 and GCS; Azure still not»). Статический аудит этого прогона не видит — но код и BACKLOG видят.

**Что реально осталось (и почему это не P1):**
- нет *повторяемой* (регрессионно-защищающей) полосы для generation-диалекта GCS — только фейк-клиент в gtest и ручная live-валидация от 2026-07-03. Аналогично Azure не начат вовсе;
- нет `Mode::Native` строки в contract-suite (то есть родовой контракт бэкенда прогоняется только над эмуляцией/локалью, хотя прикладной путь над S3 покрыт полосами). Плюс смежное: `Mode::Native` тесты сейчас пишут в cwd процесса и одна ассерция там вакуумна — уже заведено как `{#native-mode-cwd-litter}` (`BACKLOG/testing-and-ci.md:55`).
Оба остатка — про отсутствие повторяемого покрытия уже провалидированных путей, при том что неподдерживающий стор отбивается громко (`NOT_IMPLEMENTED` на mount), а не портит данные молча. Поэтому P2 (tracked post-release), не P1.

**Покрытие в BACKLOG (новый пункт не нужен):**
- `docs/superpowers/cas/BACKLOG/testing-and-ci.md:19`, секция `{#tests}`, пункт `[review #14] highest-risk coverage gaps` — дословно: «No `Mode::Native` (real S3/GCS wire) contract-suite row» — это ровно первый анкер находки. У пункта нет собственного `{#...}`-анкера, ссылаться можно на `#tests` + тег `[review #14]`;
- `docs/superpowers/cas/BACKLOG/formats-and-storage.md:22` (`{#backends}`) — `[GATE #1: Azure]`, закрывает real-store-часть (AWS+GCS сделано, Azure — открытая нога релизного гейта);
- `docs/superpowers/cas/BACKLOG/formats-and-storage.md:23` — `[GCS production-grade follow-ups]`, включая «`gcp_oauth` dialect probe validation against live GCS» — это и есть GCS-часть остатка.

**История:** CAS-полосы в CI существуют и лишь переносились между конфигами (`9ad3e15b688` «cas: ci — move CAS ParamSets to AltinityJobConfigs»); generation-диалект и его тесты пришли с `5d7f26274cb` «Route CAS metadata and delete through GCS generations» и `9b887ac8886` «Bind GCS CAS writes to exact response generations». Именно эти коммиты и делают формулировку «ships unverified» устаревшей/неверной; закрыт не сам gap (полосы для GCS по-прежнему нет), а его сформулированное следствие.

## CAS-062 — SQL-путь FSCK действительно counts-only без дедлайна и скоупа, но это уже отслеживается как CAS-049; «нет пути ремонта нигде» неверно, а исключение meta/body-счётчиков из `clean()` — by-design; реальный остаток — эти два счётчика не рендерятся ни на одной поверхности. (частично, P3) {#cas-062}

Анкеры находки указывают на снапшот `CA/...`; на HEAD файлы лежат в
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/` (перенос — `592b9b83568`).
Номера строк из находки устарели: `runFsckNow` не на `:739-745`, а на
`ContentAddressed/ContentAddressedMetadataStorage.cpp:1051-1064`; `Tools/CasFsck.h:114` — не сигнатура
`runFsck`, а середина `FsckReport` (сама сигнатура — `Tools/CasFsck.h:269-271`).

ЧТО ПОДТВЕРЖДАЕТСЯ.
1) SQL-путь действительно counts-only и без ограничителей:
`src/Interpreters/InterpreterSystemQuery.cpp:2599` — `ca->runFsckNow(/* detail= */ false)`, и
`ContentAddressedMetadataStorage.cpp:1063` — `Cas::runFsck(*store(), detail)`, т.е. не передаются ни
`on_progress`, ни `deadline`, ни `partial_on_deadline`, ни `namespace_prefix`, хотя CLI их передаёт
(`programs/disks/CommandFsck.cpp:27-40,67`: `--detail`, `--timeout` (по умолчанию 600 с),
`--namespace`, `--partial`). Скан идёт под `lifecycle_mutex`, взятым на всё время
(`ContentAddressedMetadataStorage.cpp:1055`), и не опрашивает отмену запроса, т.е. `KILL QUERY` и
`max_execution_time` игнорируются.
2) `clean()` действительно не включает `meta_without_body` / `body_without_meta`:
`Tools/CasFsck.h:238-244` (`kFsckHardFindings` — 5 терминов, этих двух там нет).

ЧТО УЖЕ ОТСЛЕЖИВАЕТСЯ (дубликат).
Пункт 1 целиком покрыт анкером
`docs/superpowers/cas/BACKLOG/operability-and-introspection.md`{#lifecycle-verbs-wait-out-uncancellable-scans}
(запись «Lifecycle verbs wait out an uncancellable GC round or FSCK scan (2031-triage CAS-049)»,
строки 128-160): там дословно зафиксировано, что SQL FSCK не передаёт ни один из bounding-параметров
CLI, держит `lifecycle_mutex` и не проверяет отмену, и что owed — дедлайн из `max_execution_time` +
`partial_on_deadline` + опрос отмены. Масштабная часть («fsck не доходит до конца на ~30 GiB») —
`gc.md`{#fsck-scale-timeout} и {#fsck-large-pool-fixed} п.(c). Ничего нового CAS-062 здесь не
добавляет.

ЧТО ФАКТИЧЕСКИ НЕВЕРНО.
a) «no repair path exists anywhere». Путь ремонта существует и вызывается из SQL: `SYSTEM CAS GC
REBUILD` (`InterpreterSystemQuery.cpp:2545-2585`, `runGcRebuildNow`) — именно он лечит класс
`stale_edge`/повреждённого in-degree-состояния, о чём сам `FsckClass::StaleEdge` и говорит
(«Only a full rebuild of the in-degree state can», `Tools/CasFsck.h:47-52`); слотовые верб-ы —
`SYSTEM CAS FORGET` и `DROP POOL MEMBER` (`Tools/CasDecommission.cpp`). Отсутствует именно
`fsck --repair` для повреждённого `_ckpt`, и это отслеживается как
`gc.md`{#ckpt-damage-no-repair-path} (п. (b): «there is NO repair path … candidate fixes = `fsck
--repair` …»). Утверждение «в `Tools/` нет функции ремонта» верно буквально, но вывод «нигде» —
нет.
b) «pool with body-without-meta or meta-without-body residue reports clean» — верно как факт, но
описано как дефект, тогда как это документированное by-design: `Tools/CasFsck.h` (поле
`meta_without_body`) объясняет, что GC удаляет тело ПЕРВЫМ и `.meta` — позже, на bounded
error-suppressed пуле, поэтому один LIST законно видит body-less `.meta`, и «no finite grace makes a
persistent one hard evidence»; `body_without_meta` — «benign, NOT a dangle»
(`Tools/CasFsck.cpp:1029-1033`). Позиция закреплена тестами:
`src/Disks/tests/gtest_cas_fsck.cpp:1238,1244` («advisory — excluded from clean()») и `:1257-1259`.
Это не «crash-residue counters», а advisory-класс; отказ считать их hard-findings — не тихая порча, а
сознательный выбор.
c) «the SQL path can tell an operator that the pool is corrupt but never which keys» — верно, но это
явно документированный YAGNI: `InterpreterSystemQuery.cpp:2426-2429` («Named UInt64 columns only, no
DETAIL keyword (YAGNI — the offline `clickhouse-disks cas-fsck --detail` applet already covers
per-object listing)»). При этом SQL-строка НЕ является подмножеством находок: все 5 терминов `clean`
там есть (`:2441-2470`, `:2489-2493`), плюс `unchecked`, janitor-* и byte-счётчики.

РЕАЛЬНЫЙ ОСТАТОК (не отслеживался).
`meta_without_body`/`body_without_meta` считаются (`Tools/CasFsck.cpp:1043,1046`), но не выводятся
НИГДЕ: их нет в `formatFsckSummary` (`Tools/CasFsck.cpp:1155-1174`), нет в
`contentAddressedFsckColumns`/`appendContentAddressedFsckRow`
(`InterpreterSystemQuery.cpp:2433-2478,2482-2504`), нет в `programs/disks/CommandFsck.cpp`, и в
`detail`-режиме на них не создаётся ни одной `FsckObject`-строки (парный цикл только инкрементирует).
Единственный читатель вне gtest — отсутствует. Соответственно комментарий в `Tools/CasFsck.h`
(«Counted and reported; excluded from `clean()`») ложен в половине «reported» — тот же класс, что
{#fsck-rule-restated-in-unfenceable-prose}. Записано новой секцией
`docs/superpowers/cas/BACKLOG/operability-and-introspection.md`{#fsck-meta-body-counters-unrendered}
(оставлено незакоммиченным): либо рендерить оба счётчика (summary-строка + SQL-строка + detail-строка
с хешем), либо удалить счётчики вместе с комментарием.

История: SQL-верб появился в `b335f784581` («dormant-only SYSTEM CONTENT ADDRESSED FSCK …»),
`--namespace`/`--partial` в CLI — `15436aa3e07`, парность meta/body — `dadac45da92`,
`kFsckHardFindings` — `4e19cfe08e7`.

ИТОГ: подтверждённая часть — дубликат CAS-049 (P2, уже отслежен); by-design часть переоценке не
подлежит; новый остаток — невидимость двух advisory-счётчиков, P3, не блокер релиза (громкие findings
рендерятся, тихой порчи нет).

## CAS-063 — Порядок «дропы namespace → снятие слота» и фильтр `/mount` в `listMounts` подтверждаются, но «повторный запуск не может починить» — фактически неверно: resume-путь есть в коде и закреплён тестами; остаётся только невидимость owner-only слота в `system.content_addressed_mounts`. (частично, P3) {#cas-063}

Анкеры находки — из снапшота `CA/...`; на HEAD это
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp` и
`.../Pool/CasServerRoot.cpp` (перенос — `592b9b83568`). Номера строк по декоммиссии почти совпали
(дропы namespace — `:159-211`, хвост снятия слота — `:244-455`, удаление `mount`/`epoch` — `:372-373`,
tombstone owner — `:408-435`); `listMounts` не на `:606-649`, а на `Pool/CasServerRoot.cpp:751-797`.

ЧТО ПОДТВЕРЖДАЕТСЯ (как код-форма).
1) Порядок именно такой: сначала `admin->dropNamespace(life)` по каждому owned namespace
(`Tools/CasDecommission.cpp:188,194`), затем дренаж manifest-debris/staging/mountpoint (`:219-242`),
и только потом снятие слота (`:267-447`).
2) Удаление мутабельных контрольных объектов идёт до tombstone: `mount` затем `epoch`
(`:372-373`), liveness-recheck (`:375-403`), и лишь потом CAS-перезапись `owner` с
`retired_at_ms` (`:408-435`).
3) `listMounts` перечисляет только ключи, кончающиеся на `/mount`
(`Pool/CasServerRoot.cpp:754-763`), и `system.content_addressed_mounts` строится из него
(`src/Storages/System/StorageSystemContentAddressedMounts.cpp:163`). Значит слот, у которого остался
только `owner`, строки в представлении не имеет.

ЧТО ФАКТИЧЕСКИ НЕВЕРНО — «a re-run cannot repair, because the capture precondition no longer holds».
Resume-путь спроектирован ровно под это окно:
- `Pool::openForDecommission` берёт uuid жертвы из `owner`-объекта, а при его отсутствии — из
  mount-лизы («partial hand-cleanup: adopt from the lease»), и только при отсутствии обоих отказывает
  с `BAD_ARGUMENTS`, указывая на `cas-fsck` (`Pool/CasPool.cpp:820-828`). В обсуждаемом окне `owner`
  ЕЩЁ ЕСТЬ и не tombstoned, т.е. это самый благополучный вход.
- Отсутствующий `epoch` при отсутствующем/терминальном `mount` — это выделенная политика
  `EpochMintPolicy::DecommissionRecovery` (`Pool/CasPool.cpp:578-582`,
  `Pool/CasServerRoot.cpp:221-247`): живая лиза — громкий отказ `ABORTED`, терминальная — минт
  заведомо отличного эпоха; при `mount` отсутствующем (`ProbeOutcome::KeyAbsent`,
  `CasServerRoot.cpp:217-218`) идёт обычный fresh-bootstrap. Поддеревo к этому моменту пусто (дренажи
  прошли), поэтому guard `serverRootSubtreeEmpty` (`CasServerRoot.cpp:196-201`) не срабатывает.
  Далее повторный прогон снова доходит до `:244-447`: victim не владеет ни одной catalog-строкой →
  `warnings` пусты → слот снимается и `owner` tombstone-ится.
- Закреплено тестами: `src/Disks/tests/gtest_cas_decommission.cpp:1325`
  (`MidRetirementCrashResumesViaMountLeaseFallback` — вручную сносят `epoch`+`owner`, повторный
  прогон доводит снятие слота до конца), `:1054`
  (`SuccessorReclaimAfterEpochDeleteKeepsOwnerAnchor`), `:1231` (`FailedDrainKeepsSlotThenResumes`),
  `:1267`, `:987` (`RemovesMutableSlotAndRefusesTombstonedRerun`).
- BACKLOG прямо фиксирует, что «the general decommission two-phase-heal flow (verified separately)»
  уже проверен (`BACKLOG/mounts-and-lifecycle.md`, запись
  {#decommission-successor-mount-race}).
Кроме того, любой незакрытый шаг хвоста не молчит: неудача tombstone даёт warning «rerun the command
to retry» (`Tools/CasDecommission.cpp:431-434`), неполный дренаж — `LOG_WARNING` «mount slot kept
(terminated) — re-run the command to finish» (`:451-453`). Это fail-closed громкий путь, не тихая
порча.

ЧТО BY-DESIGN — «the only way to clear a dead member's mount slot is a verb that first erases that
member's data». Слот и есть якорь владения: снятие слота прямо запрещено, пока в каталоге остаётся
хоть одна строка жертвы (`Tools/CasDecommission.cpp:250-265`: «upcoming GC rounds perform the final
cleanup — re-run this command afterwards to retire the slot»), а `Removing`-namespace без своего
`_ckpt` — `CORRUPTED_DATA` (`:177-183`). То есть «сначала данные, потом слот» — это инвариант, а не
дефект последовательности; «неразрушающей» альтернативой было бы перенос владения namespace другому
server root, и это отдельная, уже описанная тема с зафиксированным ограничением
(`BACKLOG/mounts-and-lifecycle.md`{#life-epoch-monotone-per-server-root}: «Pool-member decommission is
where this would be introduced»). Смежное уже отслеживается: трение вокруг определения «мёртв» —
`BACKLOG.md`{#decommission-wrong-predicate}; гонка с успешным сукцессором в хвосте снятия —
{#decommission-successor-mount-race} (и сознательно принятое окно описано в самом коде,
`Tools/CasDecommission.cpp:362-370`); выбор жертвы по префиксу — `BACKLOG.md`{#nested-srid-decommission};
сценарная карта «kill the command mid-run at each phase, then resume» — `BACKLOG/testing-and-ci.md`
(B200 follow-up), т.е. resume-поведение стоит в очереди на soak-проверку, а не отсутствует.

РЕАЛЬНЫЙ ОСТАТОК (не отслеживался): единственная невыдуманная и неотслеженная половина — п.3, т.е.
owner-only слот не виден в `system.content_addressed_mounts` (поиск по BACKLOG по `cas_mounts` /
`owner anchor` совпадений не дал). Записано новой секцией
`docs/superpowers/cas/BACKLOG/mounts-and-lifecycle.md`{#owner-only-slot-invisible-in-mounts}
(оставлено незакоммиченным): выдавать строку для слота с `owner`/`epoch` без `mount` со состоянием
вида `retiring`/`half-retired`. P3 — чистая наблюдаемость, состояние самопочиняемое повторным
запуском той же команды.

## CAS-066 — Форма кода подтверждена (режим выбирается по типу хранилища, ручки нет, лог на INFO) — это осознанное проектное решение с записанным обоснованием; риск «два сервера на одном локальном пути» уже отслеживается как doc-долг B26/B135, а под-претензия про read-only монтирование последствий не имеет. (by-design, P2) {#cas-066}

**1. Выбор режима по типу хранилища — подтверждено, by design.**
`ContentAddressedMetadataStorage.cpp:688-691`: `const auto mode = object_storage->getType() == ObjectStorageType::Local ? Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess : Cas::ObjectStorageBackend::Mode::Native;`. Никакого override нет: в списке настроек `ContentAddressedSettings.cpp:63-92` (`LIST_OF_CONTENT_ADDRESSED_SETTINGS`) нет ни одного ключа про режим бэкенда — претензия аудита («no mode setting in ContentAddressedSettings.cpp») верна буквально.

Но отсутствие ручки здесь — не дефект, а следствие того, что режим есть свойство *возможностей* хранилища, а не вкуса оператора, и обе «неправильные» комбинации закрыты:
- Native над не-S3 хранилищем (Azure/HDFS/Local) отклоняется на монтировании: `CasObjectStorageBackend.cpp:94-110` — `supportsRetryProfile(SingleAttempt)` == false ⇒ `throw Exception(NOT_IMPLEMENTED, ... "refusing to mount writable")`. Это громкий fail-closed, не тихая порча.
- Emulated над S3 недостижим по конструкции (ветка выбирается только для `ObjectStorageType::Local`).

**2. Уровень лога INFO — подтверждено, решение зафиксировано в коде с обоснованием.**
`ContentAddressedMetadataStorage.cpp:693-712`: комментарий прямо объясняет, почему это INFO, а не WARNING — inline `disk(... object_storage_type=local ...)` открывается на query-треде, и WARNING при дефолтном `send_logs_level=warning` попадает в stderr клиента и валит каждый такой тест (~15 CA-over-local stateless-тестов). Там же указан и путь на будущее («a future `system.warnings` entry could restore a louder, test-safe signal»). Само сообщение (`:707-712`) явно говорит: «safe ONLY for a single server… Do NOT share this pool path between multiple ClickHouse servers (e.g. a shared/NFS mount): the CAS/GC invariants would break silently».

**3. Ядро претензии — два сервера над одним локальным путём.**
Механизм реален: эмуляция условных операций живёт целиком в процессе — состояние токенов это поля экземпляра бэкенда (`CasObjectStorageBackend.h:205-218`: `emu_mutex`, `emu_token_state`, `emu_token_expiry`), а `emuWrite` (`CasObjectStorageBackend.cpp:532-546`) это обычная `writeObject`-перезапись без всякой атомарности. Значит exclusivity mount-lease между двумя процессами не обеспечивается, и capability-probe этого не поймает (каждый процесс проходит его в одиночку — ровно то, что написано в комментарии `:695-697`). Претензия «nothing refuses the configuration» верна.

Однако это уже отслеживаемый, осознанно принятый класс, а не новая находка: `docs/superpowers/cas/BACKLOG/formats-and-storage.md:28-34` — раздел «Local / emulated backend» `{#local-backend}` с общим корнем («`LocalObjectStorage` writes are plain `O_TRUNC` file writes — no atomic PUT, no conditional-write enforcement… Nothing in this section affects S3/GCS production pools»), и конкретно `:47-49` **[B26 / B135]** — «local / NFS / shared-fs as a first-class backend — DESIRABLE — Unit-tested over `LocalObjectStorage`; needs server-level doc + the put-if-absent atomicity caveat (racy multi-writer on local/NFS) + **multi-mount safety notes**», а также `:50-56` **[B66a]** — «racy multi-writer on local/NFS stays documented-unsafe».

Реальный остаток — только документационный, и он ровно тот, что описан в B26/B135: `docs/en/antalya/cas/quick-start.md:14-46` подаёт `<object_storage_type>local</object_storage_type>` как рекомендованный первый конфиг («needs nothing beyond a `ClickHouse` binary») и **не** несёт оговорки «single server only, никогда не разделяемый/NFS путь». Оговорка есть в серверном логе на INFO и в комментариях кода, но не на странице, которую читает оператор. Отдельный BACKLOG-пункт не добавляю: это буквально нераскрытая часть уже существующего B26/B135 (`{#local-backend}`).

**4. Под-претензия про read-only монтирование — форма верна, следствие недостижимо.**
Верно, что гейт стоит только на writable-пути: весь блок с `runCapabilityProbe`/`checkConditionalWriteSingleAttemptSupport` находится внутри `if (!config.read_only)` (`CasPool.cpp:381`, вызовы на `:465-479`), плюс отдельный вызов на `skip_access_check`-ветке (`CasPool.cpp:479`) и в decommission (`CasPool.cpp:834`).

Но вывод аудита («read-only mounts never reach the check» как проблема) не следует: `read_only` берётся из самого хранилища — `ContentAddressedMetadataStorage.cpp:791`: `read_only = object_storage->isReadOnly();`, и мутирующая поверхность при этом закрыта наглухо: `ContentAddressedMetadataStorage.cpp:1109-1113` — `checkNotReadOnly` бросает `READONLY`. Также `pool_config.background_watermark = (context != nullptr) && !read_only` (`:751`), GC не стартует (`:858`), S3-staging не поднимается (`:820`), и `PoolMeta::createOrValidate` вызывается с `allow_mint=!config.read_only` (`CasPool.cpp:496`). Условные записи, которые и защищает `SingleAttempt`-профиль (`conditionalWriteSettings`, `CasObjectStorageBackend.cpp:821-836`), на read-only монтировании не выполняются вообще. То есть пропуск проверки ничего не открывает — это классический пример «реальная форма кода + недостижимое последствие».

**Итог:** тихой порчи в поддерживаемых конфигурациях нет; неподдерживаемая конфигурация (Native над не-S3) отклоняется громко; единственный настоящий остаток — user-facing оговорка про single-server для local-бэкенда, уже учтённая в `{#local-backend}` (B26/B135). Ставлю P2 (doc-долг с высокой ценой ошибки, но низкой вероятностью и без кода-фикса), не блокер релиза.

## CAS-067 — Первая половина (грубая гранулярность mtime даёт валидный устаревший токен) закрыта mtime-quantum-дисамбигуатором ещё 2026-07-18 и покрыта тестом; вторая (перекос часов ломает истечение token-state) реальна, но это только утечка памяти в emulated-режиме без последствий для корректности — заведён новый MINOR-пункт. (частично, P3) {#cas-067}

**Общая рамка.** Всё описанное относится ИСКЛЮЧИТЕЛЬНО к `Mode::EmulatedSingleProcess`, то есть к `LocalObjectStorage` (`ContentAddressedMetadataStorage.cpp:688-691`), который по заголовку сам себя объявляет «tests and local development ONLY» (`Backend/CasObjectStorageBackend.h:39`). Пулы на S3/GCS не затронуты. Класс «INTEGRITY» здесь завышен.

**Факт 1 (верно): токен эмуляции засеян mtime.** `ObjectStorages/Local/LocalObjectStorage.cpp:391` и `:427`: `object_metadata.etag = std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());`, где `time = fs::last_write_time(path)`. Этот etag и есть вход `emuMintToken` (`Backend/CasObjectStorageBackend.cpp:532-546`, `:547-552`).

**Претензия A — «two rewrites are indistinguishable and a stale token validates» — STALE (закрыта).**
Ровно этот сценарий закрыт mtime-quantum-гвардом в `emuMintToken` (`Backend/CasObjectStorageBackend.cpp:554-580`):
```
if (it != emu_token_state.end() && it->second.first == etag) {
    if (just_wrote) ++it->second.second;
    const String value = it->second.second == 0 ? etag : etag + "#" + std::to_string(it->second.second);
```
То есть если etag не продвинулся, а это была ЗАПИСЬ, per-key дисамбигуатор инкрементируется, и новая инкарнация получает `etag#N` — значение, заведомо отличное от токена предыдущей. Запись для живого ключа никогда не удаляется из `emu_token_state`: единственное место, где запись ставится на истечение, — `deleteExact` (`:1050-1057`), а сам sweep обрабатывает только записи из очереди удалений (`:497-521`). Значит для перезаписи живого ключа гвард срабатывает всегда.

Закрыто коммитами `7fcb72050e7` (2026-07-18, «cas: emu backend — etag-seeded tokens (closes triage 19c restart collision …)») и `cbdd8493e14`/`08ea8d1200e` (тот же день, границы состояния). Failing-first покрытие существует и живо: `src/Disks/tests/gtest_cas_backend.cpp:708-762`, `TEST(CASObjectStorageBackend, EmuTokenDisambiguatesSameEtagRewrite)` — двойник `FixedEtagLocalObjectStorage` возвращает постоянный etag `"same-quantum"` (модель именно «filesystem/clock whose mtime resolution is too coarse»), и тест утверждает `EXPECT_NE(put1.token.value, put2.token.value)` плюс `deleteExact` старым токеном ⇒ `DeleteOutcome::Kind::TokenMismatch` (`:760-762`). Ситуация delete+recreate в том же кванте тоже покрыта: `:847-885` (`DeleteExactErasesEmuTokenStateOnlyWhenEtagIsComfortablyOld`) ожидает `recent_etag + "#1"`. Итог: утверждение аудита о «stale token validates» на HEAD факт-неверно.

**Претензия B — перекос часов ломает истечение — ЧАСТИЧНО верна, но следствие переоценено.**
Условие истечения (`Backend/CasObjectStorageBackend.cpp:447-464`) сравнивает etag, пришедший ОТ ХРАНИЛИЩА (mtime), с локальными `system_clock`-наносекундами (`emuNowNs`, `:479-486`): `return now_ns > etag_ns && (now_ns - etag_ns) >= EMU_TOKEN_STALE_AGE_NS;` (2 s, `:439`).
- mtime от часов, идущих ВПЕРЁД (NFS/CIFS-сервер): `etag_ns > now_ns` всегда ⇒ признак «stale» не наступает никогда. `deleteExact` уходит в ветку удержания (`:1050-1057`), а sweep после прохождения возрастного барьера снимает запись из очереди, НО не стирает запись карты (`:513-520`). Значит `emu_token_expiry` остаётся ограниченной, а `emu_token_state` теряет по одной записи на каждый удалённый ключ до конца жизни экземпляра бэкенда. Это реальная (постоянная) утечка.
- локальные часы шагнули НАЗАД: срабатывает `if (now_ns <= candidate.queued_at_ns || now_ns - candidate.queued_at_ns < EMU_TOKEN_STALE_AGE_NS) break;` (`:512-513`) — sweep стоит целиком. Но `queued_at_ns` — локальный штамп, так что состояние само рассасывается, когда локальные часы обгонят его. Формулировка аудита «pruning never fires … permanently» для этой ветки неверна; «permanently» справедливо только для случая future-dated mtime, и только для карты, не для очереди.
- Корректность не страдает ни в одном из случаев: удержанная запись максимум приводит к выдаче `etag#N` (по-прежнему уникальный токен), а recreate с продвинувшимся etag перезаписывает запись целиком (`:577-579`). Тихой валидации устаревшего токена из этого не следует.

Цена — только память: ~100 B на удалённый ключ, в режиме, который работает в CI/локальной разработке в одном процессе. Верхняя граница на нормальных (не перекошенных) часах пинается тестом `EmuTokenStateEventuallyPrunesDistinctShortLivedKeys` (`gtest_cas_backend.cpp:888-919`, 128 короткоживущих ключей ⇒ `EXPECT_LE(..., 24)`), так что регресс базового бюджета исключён.

**BACKLOG.** Существующего покрытия именно для этого не нашлось: раздел `{#local-backend}` (`docs/superpowers/cas/BACKLOG/formats-and-storage.md:28-64`) описывает неатомарность локальной записи, `[emulated list-token contract]`, `B26/B135`, `B66a` — про clock-skew в истечении `emu_token_state` там ничего; поиск по `emu_token`/`clock skew`/`mtime` по `BACKLOG.md` и `BACKLOG/*.md` дал только несвязанное (`BACKLOG.md:324` — CLOCK SKEW CAVEAT про `expires_at_ms` в mount-lease, другой механизм). Поэтому добавил (незакоммиченным) новый раздел `docs/superpowers/cas/BACKLOG/formats-and-storage.md` → `{#emu-token-state-clock-skew-leak}` с механизмом, разбором двух ветвей перекоса, направлением фикса (решать истечение по локальному монотонному `queued_at_ns`, уже лежащему в `EmuTokenExpiry`, вместо etag хранилища; либо ограничить карту по размеру как backstop) и указанием на готовые тестовые двойники.

P3: emulated-режим, не продакшн, без порчи данных, только ограниченный рост памяти при перекошенных часах.

## CAS-068 — Форма кода реальна и осознанна (расхождение с четырьмя «сиблингами» зафиксировано в коде и в коммите `4f4f93c6bc6`), но заявленный триггер (`promoteStaged`/`resurrect` → `NOT_IMPLEMENTED`) в этой полосе недостижим, а последствие — громкий fail-closed wedge, а не порча; остаётся только потеря диагностики. (by-design, P3) {#cas-068}

Якоря устарели: на HEAD это `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:305` (`CasRequestController::putIfAbsentControlled`), точка классификации — `:356-361`; «сиблинги» — `:442-453` (`conditionalCreateControlled`), `:523-531` (`putOverwriteControlled`), `:605-612` (`putIfAbsentControlledMutable`), `:691-703` (`slotOccupy`).

(1) Форма кода — ПОДТВЕРЖДЕНА.
`CasRequestControl.cpp:358-361`: `catch (const std::exception & e) { attempt_outcome = classifyConditionalWriteResult(e); }` — ни `throw`, ни лога. Четыре сиблинга действительно сначала перебрасывают: `:452-453` `if (const auto * db_e = dynamic_cast<const Exception *>(&e); db_e && isDeterministicLocalFailure(db_e->code())) throw;`, и то же на `:527-528`, `:608-609`, `:699-700`. Множество кодов — `:114-118`: `LOGICAL_ERROR`, `NOT_IMPLEMENTED`, `BAD_ARGUMENTS`, `CORRUPTED_DATA`.

(2) Это ОСОЗНАННОЕ решение, а не пропуск. Расхождение документировано прямо в коде: `:447-451` — «This deliberately differs from `putIfAbsentControlled`'s everything-Unresolved: that lane's byte-exact resolve makes retrying any unproven error harmless, while retrying a broken source/mode/encode here is pure noise»; и в заголовке `Backend/CasRequestControl.h:524-527` — «…never `putIfAbsentControlled`'s (that method predates this convention)». История: `git log -S"everything-Unresolved"` → `4f4f93c6bc6` «cas: deterministic caller bugs propagate instantly from the create retry loop», тело коммита: «`putIfAbsentControlled` (the byte-exact ref/manifest lane) is deliberately unchanged». То есть позиция автора зафиксирована; пересматривать её не требуется. Тесты знают об этой асимметрии по имени: `src/Disks/tests/gtest_cas_ref_wedge_every_attempt.cpp:148-152` — «…which `slotOccupy` rethrows but `putIfAbsentControlled` (no such special case) merely classifies Unresolved».

(3) Заявленный ТРИГГЕР — ФАКТИЧЕСКИ НЕВЕРЕН. `putIfAbsentControlled` выполняет ровно один вид попытки — `backend->putIfAbsent(key_s, bytes_s)` (`:348`); ни `promoteStaged`, ни `resurrect` в неё не попадают. Единственный вызов `promoteStaged` во всём дереве — `Pool/CasPartWriteTxn.cpp:605`, внутри лямбды `one_attempt`, которая уходит в `store->stagingConditionalCreate(key, one_attempt)` (`:623`), т.е. в `conditionalCreateControlled` — ту самую ветку, которая `NOT_IMPLEMENTED` перебрасывает (`:452-453`), о чём прямо сказано в комментарии `CasPartWriteTxn.cpp:588-593`. `resurrect` вне бэкенда не вызывается вообще (grep по `->resurrect(`). Вторая половина триггера — «any decode failure inside the conditional write» — тоже неверна: байты приходят в метод уже закодированными от вызывающего (`Pool/CasRefLedger.cpp:246`, `:3559`, `:4327`), внутри контроллера никакого decode нет; единственный decode-подобный шаг — `resolveByExactGet`, и он не «глотает», а бросает `CORRUPTED_DATA` наружу вызова (`:299-302`).

(4) Реально достижимые детерминированные исключения на этой полосе и их последствие. Единственный найденный путь — `ObjectStorageBackend::tokenFromWriteResult` (`Backend/CasObjectStorageBackend.cpp:874-886`), бросающий `CORRUPTED_DATA` на GCS-диалекте `Generation`, когда успешная запись вернула невалидную generation. Это происходит ПОСЛЕ того, как объект уже записан, поэтому проглатывание здесь не вредит, а лечит: `resolveByExactGet` (`:271-302`) читает по точному ключу свои же байты и возвращает `Committed` с токеном из GET. Если же GET тоже не удался — вызов возвращает `Unresolved`, и ledger уходит в wedge: `Pool/CasRefLedger.cpp:3869-3919` — lane остаётся pending, вызывающие получают retryable `NETWORK_ERROR` («retry later»), id не пересоздаётся вслепую (`unresolvedProvesNothingWasSent` истинно только для `NoAttemptSent`, `:3896`). То есть худший случай — громкий, самоочищающийся fail-closed wedge (следующий flush закрывает его условным CREATE, см. комментарий `:3882-3890`), а не тихая порча. Заявление аудита «wedges the ref lane into recovery instead of surfacing the bug» верно лишь в части «bug not surfaced»; «instead of» подразумевает выбор между порчей и wedge, которого здесь нет.

(5) Что реально осталось (P3, чисто операбельность): исключение попытки нигде не логируется — `classifyConditionalWriteResult` (`:43-62`) логов не пишет, catch тоже, а сообщение wedge несёт только `describeUnresolvedReason` (`CasRefLedger.cpp:3919`). Поэтому у полосы, исчерпавшей `max_attempts`, первопричина (код ошибки S3, таймаут, сокет — либо детерминированный локальный баг, который сиблинги бы перебросили) не видна ни в одной строке лога. В BACKLOG такого пункта не было (grep по `putIfAbsentControlled` в `BACKLOG.md` и `BACKLOG/*.md` даёт только `gc.md`{#ckpt-neverborn-gc-backstop}, про контракт `CORRUPTED_DATA`, не про эту тему). Добавлен новый (незакоммиченный) раздел `docs/superpowers/cas/BACKLOG/operability-and-introspection.md` {#putifabsent-swallowed-attempt-cause} с точными ссылками и предложением rate-limited лога в точке классификации. Менять сам контракт (добавлять сюда rethrow) не предлагаю: это протокольно-поведенческое изменение полосы ref-лога, отдельно зафиксированное как осознанное в `4f4f93c6bc6`.

## CAS-069 — Пустые catch существуют и причину действительно теряют, но «неотличимо от порчи → полный rebuild → вечные орфаны по CAS-025» не выводится: rebuild запускается только руками (`SYSTEM CAS GC REBUILD`), а на undecodable-ветке он не сбрасывает holds, а находит seal перечислением и отказывается с `CORRUPTED_DATA`; про `stoull`-катчи утверждение о недосчёте `max_gen` неверно. (частично, P3) {#cas-069}

Якоря устарели. На HEAD: пустой catch вокруг decode `gc/state` — `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:3874-3883` (внутри `Gc::rebuildBaseline`, `:3849`); пустые `stoull`-катчи под `gc/gen/` — `:1484-1493` (`newestFoldSealRef`), `:1616-1622` (`probeGenerationForSeal`), `:4090-4099` (нумерация `max_gen`).

(1) Форма кода — ПОДТВЕРЖДЕНА. `:3877-3883`: `try { decoded = decodeGcState(got->bytes); } catch (...) // NOLINT(bugprone-empty-catch) { /// undecodable state = scenario (а) }` — текст исключения не логируется и никуда не выносится. `healthy` остаётся `false`, поэтому проверка `if (healthy && !force)` (`:4071-4077`) не срабатывает и команда продолжает перестройку. Важно: сам GET вынесен ЗА try (`:3869`), так что сетевой сбой чтения не маскируется — он летит наружу.

(2) Заявленное последствие — В ОСНОВНОМ НЕ ВЫВОДИТСЯ. Во-первых, достижимость: `rebuildBaseline` вызывается ровно из одного места — `ContentAddressedMetadataStorage.cpp:682`, т.е. из ручной команды `SYSTEM CAS GC REBUILD [FORCE] <disk>` (`src/Interpreters/InterpreterSystemQuery.cpp:2554`, парсер `src/Parsers/ParserSystemQuery.cpp:479`); никакой автоматический путь «транзиентная ошибка → rebuild» не существует. В обычном раунде decode `gc/state` не глушится вовсе — исключение распространяется (fail-closed `CORRUPTED_DATA`; ср. `BACKLOG/gc.md`{#gc-followups} `[gc-rebuild follow-ups]`: «`mc rm gc/state` mid-soak → guard fires `CORRUPTED_DATA`»). Во-вторых, на самой undecodable-ветке holds НЕ теряются: `:3956` `if (!decoded || decoded->snap_generation == 0)` → `newestFoldSealRef()` перечислением находит новейший fold seal, и если он есть, но не читается/исчез — команда БРОСАЕТ `CORRUPTED_DATA` («GC refuses to rebuild; this pool must be recreated», `:3969-3990`). Единственный hold-free исход — пул, где объекта seal нет вообще (`:3963` `rep.virgin_by_enumeration`, счётчик `CASGCRebuildVirginByEnumeration`, `src/Common/ProfileEvents.cpp:888`). Так что цепочка «→ CAS-025 permanently orphans unreferenced blobs» опирается на уже принятый и отдельно оттрекованный по-дизайну остаток (`BACKLOG/gc.md`{#gc-followups}, пункт `[REBUILD R4 residual — manifest-less blobs unreclaimable]`; см. также вердикт `tmp/2031/results/CAS-025.md` — by-design), а не на этот catch.

(3) «MEMORY_LIMIT_EXCEEDED on a large `gc/state`» — премисса не подтверждается. `gc/state` — маленький объект состояния (единственное поле переменной длины — `msc`, ключ из страницы LIST, ограниченный длиной ключа бэкенда; строки читаются с 64 KiB `line_cap`), см. уже существующий пункт `BACKLOG/formats-and-storage.md`{#gc-state-encode-no-line-cap} с точными ссылками на `Formats/CasGcStateFormat.cpp:43`, `:66`. Тело уже полностью материализовано GET-ом до try, так что «большой `gc/state`, на котором рвётся лимит памяти при decode» — изобретённая арифметика. Теоретически `catch (...)` действительно поймал бы `MEMORY_LIMIT_EXCEEDED`, но это не тот класс, который в этой точке реалистичен, и последствие ограничено п.(2).

(4) Второе утверждение — «malformed generation key is skipped with no log, under-computing `max_gen`» — ФАКТИЧЕСКИ НЕВЕРНО в части «under-computing». Катчи на `:1490`, `:1621`, `:4096` защищают только `std::stoull`, который падает исключительно детерминированно (`std::invalid_argument`/`std::out_of_range`) — транзиентная ошибка тут невозможна, то есть «reclassify transient read failures as corruption» к ним не относится вообще. Корректно сформированный ключ генерации `gc/gen/<N>/…` разбирается всегда (`N` — десятичное число, помещающееся в uint64), поэтому пропустить настоящую генерацию и недосчитать `max_gen` эти катчи не могут; отбрасывается только посторонний мусор под префиксом, что и написано в комментариях («foreign key shape under `gc/gen` is debris, not a generation number», `:1492`, `:4098`). Отсутствие лога здесь — сознательный выбор (мусор под `gc/gen` штатно возможен после потерянной эры), а не потеря сигнала о повреждении. Отдельно: ненадёжность самого перечисления (LIST может скрыть генерацию) — это уже оттрекованный класс `BACKLOG/gc.md`{#gc-followups} `[REBUILD-SEAL-POINT-READ]` и settled-вердикт по доверию к LIST (`docs/superpowers/cas/2026-08-03-list-trust-verdict.md`), а не следствие пустого catch; в самом коде эта недоверчивость проговорена на `:1468-1472` и рефьюзом «seal above the listing's maximum» (`:1608-1612`).

(5) Что реально осталось (P3, диагностика). В `rebuildBaseline` отброшенное исключение — единственное свидетельство того, ПОЧЕМУ `gc/state` не декодировался; ни лога, ни поля в `RebuildReport` нет, поэтому оператор ручной DR-команды не может отличить настоящее байтовое повреждение от сбоя самого decode, хотя рядом в этом же файле принят обратный стиль (`:485` — `LOG_WARNING(... e.what())` для janitor, `:1897` — `tryLogCurrentException` для перепроверки condemn-маркера). Существующее покрытие в BACKLOG: `BACKLOG.md`{#damaged-object-repair} пункт 1 требует ровно этого различения (present-and-undecodable vs absent vs decodable-but-inconsistent) — но для fsck, а не для отчёта rebuild; `BACKLOG/gc.md`{#gc-followups} `[gc-rebuild follow-ups]` уже должен rebuild-у отдельную строку gc-round-log, куда причина и просится. Точного пункта про потерянную причину decode не было, поэтому добавлен новый (незакоммиченный) раздел `docs/superpowers/cas/BACKLOG/gc.md` {#rebuild-gcstate-decode-reason-unreported} — с фиксацией того, что защитная половина (перечисление seal + отказ) менять нельзя, и с явной пометкой, что `stoull`-катчи дефектом не являются.

## CAS-072 — Форма кода реальна (один слот precommit, перезаписывается без проверки), но второй `precommitAdd` на одном `PartWriteTxn` не достижим ни на одном рабочем пути — остаётся латентный инвариант без исполняемой защиты. (частично, P3) {#cas-072}

**Что подтверждается в коде на HEAD.**

1. `PartWriteTxn` держит РОВНО ОДНУ precommit-привязку: тройка полей
   `precommit_target_ns` / `precommit_final_ref` / `precommit_manifest`
   (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h:383-385`) плюс
   `precommit_state` (`:374`). Комментарий там же прямо описывает единственность и то, что поля
   «never cleared afterwards» (`CasPartWriteTxn.h:377-382`).

2. `precommitAdd` безусловно перезаписывает эту тройку на каждом вызове, никакой проверки
   `precommit_state == NotAttempted` нет:
   `Pool/CasPartWriteTxn.cpp:950-953` (`precommit_target_ns = target_ns; precommit_final_ref = ...;
   precommit_manifest = id.ref; precommit_state = PrecommitState::Uncertain;`).

3. Уборка мусора действительно пропускает только ОДНУ (последнюю) привязку:
   `Pool/CasPartWriteTxn.cpp:1426-1427`
   (`if (precommit_attempted && id.ref == precommit_manifest && id.root_namespace == precommit_target_ns) continue;`),
   а остальные staged-манифесты удаляются writer-ом exact-token (`:1429-1436`).

4. Оба «терминальных» потребителя тоже однослотовые: `abandon` добавляет удаление ровно этой одной
   привязки (`Pool/CasPartWriteTxn.cpp:1338-1352`), а деструктор передаёт mount-у cleanup-duty ровно на
   неё же (`Pool/CasPartWriteTxn.cpp:132-136`).

Итого: описанная в находке ФОРМА кода верна, и при двух вызовах `precommitAdd` первая привязка осталась бы
жить в ref-логе, а её тело было бы удалено writer-ом — то есть заявленное следствие механически вытекает
из формы.

**Что не подтверждается — достижимость.**

Ни один продуктивный путь не вызывает `precommitAdd` дважды на одном объекте:

- `ContentAddressedTransaction::publishStaging` — два места вызова, но это ВЗАИМОИСКЛЮЧАЮЩИЕ ветви:
  scratch-ветка (`ContentAddressedTransaction.cpp:357`) заканчивается `abandon()` + `st.build.reset()` и
  `return` (`:386-392`) до того, как поток дойдёт до обычной ветки (`:411`).
- Каждый `PartStaging` владеет собственным build-ом (`ContentAddressedTransaction.cpp:148-153`,
  `buildFor`), ключ карты — `(ns, ref)` (`:143-145`); в `commit()` каждая staging публикуется ровно один
  раз (`:496`, флаг `st.published` на `:315`).
- Повторный `commit()` после сбоя запрещён fail-closed: `ContentAddressedTransaction.cpp:436-438`
  («retrying a failed content-addressed transaction is not supported»), поэтому «re-stage retry» на том
  же объекте, о котором говорит находка, невозможен.
- `moveDirectory` переносит build с src-ключа на dst-ключ (`:1363-1365`), а лишний build прямо
  `abandon()`-ится (`:1377`) — всё ещё один precommit на build.
- `CachedPartFolderAccess::prepareEntries` создаёт СВЕЖИЙ build на каждый handle
  (`Parts/PartFolderAccess.cpp:474-485`), а `PreparedPartWrite` «owes exactly one terminal operation»
  (`Parts/PartFolderAccess.cpp:459-463`). Путь relink/exchange идёт через него же.

**Класс последствия завышен.** Даже в гипотетическом сценарии это не DATA-LOSS: precommit-привязка — это
интент, а не ссылка на данные, и «живой precommit без тела» обрабатывается fail-safe: fold считает такую
кромку неактивирующей и либо держит барьер, либо (когда build доказуемо мёртв по watermark-полу) прямо
пропускает её, вместо вечного clamp — `Gc/CasGc.cpp:2540-2567` («live precommit body absent AND its build
is below the watermark floor (provably dead); skip the non-activating edge instead of clamping forever»),
плюс rebuild-ветка `Gc/CasGc.cpp:4217-4241`. Так что худший исход — утечка привязки до
stale-precommit-sweep/remount и временный clamp, а не потеря данных.

**История / BACKLOG.** Ничего по этому пункту в `docs/superpowers/cas/BACKLOG.md` и
`docs/superpowers/cas/BACKLOG/*.md` не нашлось (`grep` по `precommitAdd`, `precommit_manifest`,
`one precommit` даёт только несвязанные упоминания: `BACKLOG/performance.md:202`, `BACKLOG/gc.md:155`).
Однослотовая дисциплина складывалась вместе с `PrecommitState` (мотивация — `Uncertain`-append, см.
`CasPartWriteTxn.h:283-298` и тест `gtest_cas_ref_install_safety.cpp:873-911`), но исполняемой проверки
«не более одного precommit на build» так и не появилось.

**Что реально осталось.** Латентный инвариант без защиты: будущий вызывающий код может добавить второй
`precommitAdd` и получить именно описанный молчаливый сирота-биндинг. Записал новый пункт (не
закоммичен) в `docs/superpowers/cas/BACKLOG/ref-protocol.md`, якорь
`{#precommit-add-single-slot-guard}`: сделать инвариант исполняемым (`LOGICAL_ERROR` при
`precommit_state != NotAttempted`) либо заменить тройку контейнером, по которому итерируются `abandon`,
duty деструктора и уборка мусора. Там же отмечен побочный наблюдённый момент: идемпотентный re-add
(no-op-ветка замыкания `Pool/CasPartWriteTxn.cpp:961-975`) всё равно оставляет
`precommit_state == Durable`, поэтому последующий `abandon` такого build-а добавит удаление никогда не
существовавшей привязки и упадёт по строгой ветке — fail-closed, но шумно.

## CAS-073 — Все три формы верны, но следствие изобретено: маркер по определению не является авторитетом удаления — удаляет exact-token delete по токену из retired-строки, а resurrect ротирует incarnation-tag, так что маркер «прошлой инкарнации» ничего удалить не разрешает. (by-design, P3) {#cas-073}

**Утверждение 1 — «маркер не incarnation-scoped» (форма верна, следствие ложно).**
`BlobMeta` действительно не несёт токен:
`Formats/CasBlobMetaFormat.h:28-34` (`version` / `state` / `condemn_round` / `size`). Но это явно
описанный design-выбор, а не упущение: там же, `Formats/CasBlobMetaFormat.h:10-14` — «This marker is
only a point-read hint, not the linearization point for blob lifetime: the body's in-body
`incarnation_tag` and the body's exact-token delete provide the safety guarantee… it is never authority
for deleting the body». Код это подтверждает:

- ЕДИНСТВЕННОЕ место удаления тела — `Gc/CasGc.cpp:802`:
  `backend.deleteExact(layout.blobKey(entry.ref), entry.token)`, где `entry.token` — токен, снятый
  HEAD-ом в момент condemn (`Gc/CasBlobInDegree.cpp:560-568` / `:576-584`), а не свежий HEAD. Пометка
  сайта — `Gc/CasGc.cpp:767-769` («THE SINGLE CONTENT-DELETE SITE»).
- Resurrect ротирует инкарнацию и никогда не читает умирающее тело:
  `Pool/CasPartWriteTxn.cpp:709-716` (fresh `buildHeader()` ⇒ иной ETag) и `:769-771`
  (`backend().resurrect(*payload, source.size, key, buildHeader())`), плюс INV-1 отказ читать
  condemned-тело — `Pool/CasPartWriteTxn.cpp:373-390`.
- Следовательно любой отложенный `deleteExact(t1)` по новой инкарнации даёт `TokenMismatch`, что
  обрабатывается как `Replaced` — «terminal-OK: the fresh incarnation is a live object»
  (`Gc/CasGc.cpp:826-833`, `:864-868`: meta намеренно не трогается, её уже перевёл в `Clean` writer).

Так что «marker written for a previous incarnation licenses deleting the new one» на HEAD неверно.

**Утверждение 2 — «`writeCondemnedMeta` возвращает true, когда уже `Condemned`» (форма верна,
интерпретация неверна).**
`Gc/CasGc.cpp:127-137`: при уже `Condemned` возвращается `true` без записи. Предикат гейта — не «мы
написали маркер», а «durable Condemned evidence exists», и именно это нужно gate-у: смысл маркера —
адопт-гейт writer-а (`Pool/CasPartWriteTxn.cpp:372-373` — point-read `Condemned` ⇒ ABORTED ⇒
re-upload), поэтому любой durable `Condemned` (чьей бы инкарнации он ни был) закрывает same-token
адопт. Это выписано в дереве дословно, включая ровно ту оговорку, которую находка выдаёт за дыру:
`Gc/CasGc.cpp:1874-1885` — «(`BlobMeta` carries no token, so the re-check is per-hash by design; the
two-phase pipeline + the exact-token delete carry the rest)» и `:1903-1912` — «Accepted race… This is
never destructive — the eventual exact-token delete is a no-op against the fresh token…worst case…
re-uploads once (a spurious resurrect)». In-process реестр подтверждений keyed по (hash, exact token)
(`Gc/CasGc.cpp:441-458`, `Gc/CasGc.h:959-967`) и после рестарта деградирует к синхронному
`loadMeta`-перечтению — то есть «in-process only» (`Gc/CasGc.cpp:691` из находки — это область
`scheduleMetaJob`/`scheduleCondemnMarkerWrite`) не даёт никакого небезопасного следствия, только
fail-safe перенос.

**Утверждение 3 — «спасённый blob навсегда сохраняет durable `Condemned`» (верно и намеренно).**
`Gc/CasGc.cpp:896-911`: spare намеренно НЕ трогает meta; add-only политика с выписанной причиной —
`Gc/CasGc.cpp:107-118` («GC freshness meta is ADD-ONLY… a deposed leader that cleared a spare's meta
then lost its round CAS would leave a durable stray-`Clean` over a still-condemned body… live-blob data
loss (INV_NO_LOSS)»). Это следствие закрывающего коммита `730b59cd686`
(«cas: GC freshness meta is add-only — remove spare-side clearSparedMeta (deposed-leader fix)»), то есть
удаление spare-side очистки — сознательный фикс, а не пропуск. Самозалечивание описано на месте
(`Gc/CasGc.cpp:909-910`): следующий `putBlob` по этому хэшу отказывается от same-token адопта и делает
resurrect, который единственный переводит `Condemned -> Clean` (`Pool/CasPartWriteTxn.cpp:537-556`,
причём не «best-effort»: не уложившись в 8 попыток, он бросает retry-later). Остаточная цена — один
лишний re-upload на живом, но помеченном хэше; корректность не страдает.

**Утверждение 4 — «`closeBlob` может записать замену только для entry, которую раунд уже коснулся»
(верно; последствие — fail-safe).**
`Gc/CasBlobInDegree.cpp:530-546`: supersede-ветка гейтится `cur_edges == 0 && cur_touched && peek_head`.
Если resurrect произошёл, но в этом раунде хэш не был «тронут», stale-строка со старым токеном
переносится дальше; её последующий `deleteExact` попадает в `TokenMismatch` ⇒ `Replaced` ⇒ запись
покидает конвейер, живое тело не тронуто (`Gc/CasGc.cpp:815-833`, `:864-870`). Никакого удаления «новой»
инкарнации не происходит. При этом «нетронутый resurrect» и сам по себе почти невозможен: resurrect
происходит из-за writer-а, чья precommit-кромка durable ДО наблюдения тела (EDGE-BEFORE-OBSERVE), то
есть в общем случае хэш и оказывается тронутым.

**BACKLOG / история.** Эта тема уже адъюдицирована и зафиксирована как settled position:
`docs/superpowers/cas/BACKLOG.md` `{#cas-021-followups}` («CAS-021 (issue #2207) adjudication
follow-ups», решение пользователя 2026-08-20). Там прямо сказано: «GC's gate predicate is "durable
Condemned evidence exists"… same as the already-Condemned arm at `CasGc.cpp:137`», а пункт (2) —
«Stale condemn-marker memoization — ACCEPTED RESIDUAL, do NOT fix with re-reads» с обоснованием цены
(+1 billable GET на каждую graduating-запись против бесплатных DELETE в редкой гонке). Пункт (1) того же
раздела уже планирует именно то, что осталось по CAS-073: doc/тип-патч, который делает прочтение
аудитора невозможным — в том числе cross-reference-фраза у `writeCondemnedMeta` («a foreign Condemned
marker satisfies the predicate by design, same as the `:137` arm»). Также релевантен закрывающий коммит
гейта градации `21a6051e8ff` («cas: gc — condemn marker is load-bearing: graduation gated on confirmed
durable meta (triage #4)»).

**Что реально осталось.** Кода менять нечего; остаётся только уже отслеживаемая косметика/док-работа под
`{#cas-021-followups}` (пункты (1) и (3)) — поэтому P3 и никакого нового пункта в BACKLOG не добавлял.
Дубликатом CAS-YYY внутри этой партии не помечаю, но по существу это повторение внешнего CAS-021 (issue
#2207), уже разобранного на HEAD `684161dcc03`.

## CAS-070 — Из трёх заявленных механизмов реален только один — `remount_running` латчится до создания потока, поэтому упавший спавн навсегда выключает само-ремоунт; «lost wakeup» и «нет обработчика в теле потока» на HEAD не подтверждаются. (частично, P2) {#cas-070}

Разбор по каждому из трёх заявленных механизмов (анкеры финдинга указывают старые пути `CA/Pool/...`; символы найдены в `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp` после переезда в подкаталоги коммитом 592b9b83568).

1) ЛАТЧ ФЛАГА ПЕРЕД БРОСАЮЩИМ СПАВНОМ — ПОДТВЕРЖДЕНО.
`CasMountRuntime::scheduleRemount` сначала ставит флаг, потом создаёт поток:
`CasMountRuntime.cpp:451` — `remount_running.store(true);`
`CasMountRuntime.cpp:452` — `remount_thread = ThreadFromGlobalPool([this] {...});`
Единственный сброс флага — в самом конце тела потока: `CasMountRuntime.cpp:470` — `remount_running.store(false);` (проверено grep'ом: других записей `remount_running` в дереве нет).
Конструктор `ThreadFromGlobalPool` бросает: `ThreadFromGlobalPoolImpl` → `startThreadFromGlobalPool` → `GlobalThreadPool::instance().scheduleOrThrow` (`src/Common/ThreadPool.cpp:1084`), и в комментарии там прямо сказано «If scheduleOrThrow throws, the ThreadFromGlobalPoolImpl destructor won't be called» (`ThreadPool.cpp:1082`). Точка броска — `src/Common/ThreadPool.cpp:330`: `if (CannotAllocateThreadFaultInjector::injectFault()) return on_error("fault injected");` плюс обычные `no free thread`/`shutdown` ветки.
Следствие реально и постоянно: после броска потока нет, а гейты `CasMountRuntime.cpp:444` и `:447` (`remount_shutting_down.load() || remount_running.load() || remountTerminal()`) навсегда возвращают из `scheduleRemount`. Фенс при этом уже закрыт (`tripMountLost()` вызывается ДО `scheduleRemount` в колбэке `on_lost`: `CasMountRuntime.cpp:251-253`), т.е. диск остаётся fenced closed до рестарта процесса — ровно тот LIVENESS-эффект, который описан в финдинге.
ДОСТИЖИМОСТЬ подтверждена не только «исчерпанием пула»: инъектор `cannot_allocate_thread_fault_injection_probability` включается в CI (`tests/config/config.d/cannot_allocate_thread_injection.xml`, `tests/docker_scripts/stress_runner.sh`), т.е. в stress-прогонах ветка реально берётся.
ТИХОСТЬ: исключение из `scheduleRemount` в основном пути (колбэк `on_lost` кипера) глотается пустым catch без лога — `CasServerRoot.cpp:1448-1456` (`catch (...) // NOLINT(bugprone-empty-catch)` с комментарием «a hook exception must not escape it»); второй вызывающий, `Pool::reportImpossibleInterference` (`CasPool.cpp:1472`), тоже вызывает `scheduleRemount` вне try. Т.е. оператор видит навсегда зафенсенный диск без объяснения.
Класс: fail-loud (записи падают типизированной ошибкой, потери/порчи данных нет), поэтому это не блокер релиза, но потеря само-восстановления + невидимость причины делают это трекуемым P2. Починка тривиальна: ставить `remount_running` только после успешного конструирования потока (или сбрасывать в catch) и громко логировать неудачное арминг.

2) «stopRemountThread lost wakeup» — НЕ ПОДТВЕРЖДЕНО (максимум задержка).
`CasMountRuntime.cpp:492-502`: под `remount_thread_mutex` латчится `remount_shutting_down`, затем `remount_stop.store(true)` и `remount_cv.notify_all()` вне `remount_cv_mutex`, затем join. Формально запись предиката вне мьютекса ожидания — известный анти-паттерн, но ожидание таймаутное: `CasMountRuntime.cpp:466` — `remount_cv.wait_for(lk, std::chrono::milliseconds(backoff_ms), [this]{ return remount_stop.load(); })`, а `backoff_ms` растёт максимум до 30000 (`:468`). Хуже всего — до одного интервала бэкоффа задержки join при teardown; постоянной потери пробуждения (заявленный эффект «self-healing permanently disabled») здесь нет.

3) «тело потока без обработчика» — НЕ ПОДТВЕРЖДЕНО фактически.
Тело действительно не имеет своего try (`CasMountRuntime.cpp:452-471`), но вызываемый колбэк — `Pool::tryRemountOnce`, у которого контракт «returns bool, never throws» задекларирован и обеспечен: вся рекавери-часть обёрнута (`CasPool.cpp:1083` `try` … `CasPool.cpp:1205-1219` `catch (...)` + `tryLogCurrentException`), диагностический GC-раунд отдельно защищён (`CasPool.cpp:1015-1018`: `try { return currentGcRound(); } catch (...) { return 0; }`), а шаг-0 гейт не бросает по построению: `probePoolLifecycleGate` ловит декод (`CasPool.cpp:114-123`) и опирается на `probeSentinel` → `Backend::probeSentinelRaw`, который классифицирует все ошибки в `ProbeOutcome` вместо броска (`Backend/CasObjectStorageBackend.cpp:724-780`, `Backend/CasSentinelProbe.cpp:9-12`). Так что «worker died on an exception» — это описанная форма кода с практически недостижимым следствием (остаётся только экзотика вида `bad_alloc`), и даже в этом случае join не подвешивается: `state->event.set()` стоит в `SCOPE_EXIT` (`ThreadPool.cpp:1091-1094`), а глобальный пул создан с `shutdown_on_exception=false` (`ThreadPool.cpp:1051`).

ИСТОРИЯ/BACKLOG: `git log -S "remount_running" -- src` даёт только два коммита-источника (`ba0a5231a3b` «CAS mount: self-remount after GC fence-out», `b4a24d56cf5` извлечение `CasMountRuntime`) — исправляющего коммита нет, т.е. пункт 1 не закрыт позднейшей работой. В `docs/superpowers/cas/BACKLOG.md` и `docs/superpowers/cas/BACKLOG/*.md` покрытия именно этой формы нет: смежные пункты про фенс-окно — `BACKLOG/mounts-and-lifecycle.md` {#mount-fence} (fence-window observability; fence-window blast radius) и {#P3.1 Task 6 / S13} — говорят о наблюдаемости и длительности ремоунта, но не о том, что арминг ремоунта может быть выключен навсегда.
ЧТО ОСТАЛОСЬ: только пункт 1. Добавлен новый (незакоммиченный) раздел `docs/superpowers/cas/BACKLOG/mounts-and-lifecycle.md` {#remount-running-latched-before-spawn} с анкерами и направлением фикса; там же зафиксировано, что пункты 2 и 3 финдинга не подтверждаются.

## CAS-071 — Позиция прежнего вердикта (CAS-090: by-design, latent) на HEAD в силе — `mount_keeper` меняется только под `Pool::remount_mutex` и его единственный «конкурент» отсечён конфиг-гейтом в коде; заявленный разрыв fence/deadline против `mayMutate()` фактически неверен, остаётся один косметический остаток (`pool_uuid` публикуется вне `pointer_mutex`). (by-design, P3) {#cas-071}

Ванлайнер несёт заранее заполненный вердикт «by-design (prev CAS-090)»; проверял именно то, соответствует ли позиция коду на HEAD. Соответствует. Разбор по четырём анкерам (пути в финдинге старые, `CA/...`; символы найдены после переезда 592b9b83568).

1) `mount_keeper` — обычный `unique_ptr`, перевешивается из ремоунт-потока. ФОРМА ВЕРНА, ГОНКИ НЕТ.
Объявление: `Pool/CasMountRuntime.h:379` — `std::unique_ptr<MountLeaseKeeper> mount_keeper;` (без мьютекса, без TSA-аннотации). Переприсваивание единственное: `Pool/CasMountRuntime.cpp:238` (`installKeeper`), вызывается из `Pool::tryRemountOnce`, у которого первая строка — `std::lock_guard serialize(remount_mutex);` (`Pool/CasPool.cpp:998`), а сам вызов — `CasPool.cpp:1157` (перед ним `CasPool.cpp:1147` `keeperStopBackground()`, который джойнит поток кипера — `SingleWriterSlot::stopBackground`, `Pool/CasServerRoot.cpp:1396-1408`, — так что старый объект уничтожается уже без живого потока).
Полный список читателей `mount_keeper` в дереве (grep): `CasMountRuntime.h:297` (`hasKeeper`), `CasMountRuntime.cpp:166-168` (`renewWatermarkOnce`), `:247`, `:259`, `:264`, `:269`, `:274`, `:279`, `:511-532` (`finishTeardown`). Продакшн-вызывающие: путь `open`/`mountWritable` (однопоточный, до публикации пула — `CasPool.cpp:712/720/787/797`), путь ремоунта (под `remount_mutex` — `CasPool.cpp:1147/1157/1185`) и teardown после джойнов (`CasPool.cpp:928-957`: `stopRemountThread()` → … → `keeperReset()`, с комментарием «every keeper-touching thread (renewal, remount) is joined»). Конкурентного читателя в продакшне нет.
Заявленный «конкурент» из CAS-090 — `renewWatermarkOnce` (`CasMountRuntime.cpp:162-169`, действительно без блокировки) — в проде не вызывается: `Pool::renewWatermarkOnce` (`CasPool.cpp:1233`) встречается только в gtest'ах (`src/Disks/tests/gtest_cas_gc_leak.cpp` и др.). Более того, взаимоисключение НЕ «unenforced», как формулировал CAS-090: ремоунт-поток вообще не создаётся при выключенном фоновом режиме — `CasMountRuntime.cpp:437-438` (`if (!config.background_watermark) return;`), а `renewWatermarkOnce` — это ровно тестовый режим `background_watermark=false` (`CasMountRuntime.h:55`). То есть гейт в коде.

2) «fence/deadline state is torn between the renewal thread and `mayMutate()`» — ФАКТИЧЕСКИ НЕВЕРНО.
`mayMutate` читает только атомики с acquire: `CasMountRuntime.cpp:80-84` (`mount_fence.lost.load(acquire)`, `mount_fence.deadline_boot_ms.load(acquire)`), поля объявлены `std::atomic` (`CasMountRuntime.h:80-81`). Писатели тоже атомарны и порядок задан намеренно: `setMountDeadline` (`:129-132`), `tripMountLost` (`:86-95`), `armMountFence` (`:134-147`) с явным комментарием «Open the gate LAST» + бамп `fence_generation` (`:141`). Неатомарные поля `MountFence::server_uuid`/`writer_epoch` (`CasMountRuntime.h:76-77`) в дереве только ЗАПИСЫВАЮТСЯ (`CasMountRuntime.cpp:136-137`) и никем не читаются — гонки по ним не может быть по определению (косметика: мёртвые поля).
Про `MountLeaseKeeper`: `prepareRenew` действительно мутирует `last_attempt_wall_ms`/`last_attempt_boot_ms` до взятия `state_mutex` (`Pool/CasServerRoot.cpp:902-911`, вызов из `doStart` `:1303` и `renewOnce` `:1320`), но это задокументированный однодрайверный инвариант: `CasServerRoot.h:737-741` («prepareRenew, claim, and the hooks all run on the single renewal driver thread») + `CasServerRoot.cpp:1321-1325` («renewOnce has a single driver»), а прямые вызывающие `renewOnce` вне фонового цикла — только однопоточные фазы open/remount и тестовые сиамы. `confirmed_deadline_ms` (`CasServerRoot.h:722`) читается/пишется исключительно на потоке кипера (`shouldFenceOnTransientRenewFailure` `:892-900` вызывается из `backgroundLoop` `:1434`). То, что реально шарится с teardown, уже сделано атомарным осознанно — `deposition_observed` (`CasServerRoot.h:735`, «Atomic because the keeper's background thread sets it and teardown reads it»).

3) «pool identity is published after the pool itself under a suppression, so `getPoolUUID()` can observe a half-initialized storage» — ФОРМА ВЕРНА, СЛЕДСТВИЕ НЕДОСТИЖИМО; остаётся косметика.
`ContentAddressedMetadataStorage::startup` помечен `TSA_NO_THREAD_SAFETY_ANALYSIS` с обоснованием предусловия: `ContentAddressedMetadataStorage.h:270-276` («Runs exactly once, single-threaded, strictly before this object is exposed to any other thread … pointer_mutex/gc_scheduler_mutex exist to guard concurrent access AFTER startup, which is definitionally impossible during it»). Единый publish-шаг под `pointer_mutex` реально существует (`ContentAddressedMetadataStorage.cpp:876-882`: `cas_store`/`part_access`/`gc_scheduler`), а `pool_uuid` и `conditional_copy_supported` присваиваются СРАЗУ ПОСЛЕ выхода из блокировки (`:883-884`), при том что `pool_uuid` — обычный `String` без TSA-гарда (`ContentAddressedMetadataStorage.h:641`, в отличие от `cas_store`/`part_access`/`gc_scheduler` с `TSA_GUARDED_BY(pointer_mutex)`). При заявленном предусловии (никто не наблюдает объект во время `startup`) это не гонка вовсе; читатели `pool_uuid` (`:456-469` снапшот, `:1094` текст «constructing/shutdown», `:2110` роутинг relink-токена) в любом случае требуют опубликованного пула. Остаток — чисто гигиенический: перенести две присваивающие строки внутрь того же `lock_guard`, чтобы комментарий про «single publish step» стал буквально верным. Поведенческого эффекта нет, поэтому отдельный пункт в BACKLOG не создавал (P3, косметика).

4) «the event sink can be replaced while being invoked» (`CasEventDispatcher`) — ФОРМА ВЕРНА, В ПРОДАКШНЕ НЕДОСТИЖИМО.
`EventDispatcher::setSink` меняет `sink` под `mutex` (`Pool/CasEventDispatcher.cpp:10-15`), а путь доставки читает `sink` с отпущенным мьютексом (`CasEventDispatcher.cpp:33-42`, комментарий «`sink` is set pre-traffic and never swapped concurrently with delivery»); контракт задокументирован в `CasEventDispatcher.h:38-45`. В продакшне `setEventSink` вызывается ровно один раз сразу после конструирования пула, до старта любого потока: `CasPool.cpp:502` (в `Pool::open`) и `CasPool.cpp:847` (в writer-only фабрике); ср. `CasPool.cpp:1100` («setEventSink ran long ago»). Поздние вызовы есть только в gtest'ах (`src/Disks/tests/gtest_cas_gc_rebuild.cpp:643/661`, `gtest_ca_wiring.cpp:1292/1327`). Это же остаточное место было отмечено при закрытии CAS-091 (см. прошлый раунд: `Pool/CasEventDispatcher.cpp:37-42` как residual при вердикте «fixed»), т.е. известно и принято.

ИСТОРИЯ/BACKLOG: прежний вердикт этой формы — CAS-090, «📐 by-design (latent, unenforced)» с анкерами `Pool/CasMountRuntime.h:400`, `CasMountRuntime.cpp:156-163`, `:226-249` (`tmp/clickhouse-regression/cas/docs/cas-audit-rerun-20260730/verdicts.tsv:128`, `RECONCILIATION.md:132`). На HEAD анкеры сместились (`.h:379`, `.cpp:162-169`, `:232-255`), содержательно позиция та же — и стала СИЛЬНЕЕ: взаимоисключение теперь обеспечено гейтом `background_watermark` в `scheduleRemount`. Открытого пункта в `docs/superpowers/cas/BACKLOG.md` / `BACKLOG/*.md` по этому классу нет и не требуется. Формально дубликатом CAS-090 назвать нельзя: CAS-071 склеивает четыре разные позиции, из которых одна (fence/deadline tearing) просто неверна.

## CAS-078 — Сброс курсора уборщика namespace на любой ошибке LIST реален и запинен тестом, но это только задержка реклейма (громкая, посчитанная fsck), а не потеря данных. (подтверждено, P3) {#cas-078}

Код на HEAD (пути сместились в `Gc/`, файл `Gc/CasNamespaceJanitor.cpp`):

- Механика подтверждена буквально: `Gc/CasNamespaceJanitor.cpp:21` читает `progress.state->janitor_cursor`, а LIST обёрнут в
  `Gc/CasNamespaceJanitor.cpp:22-31`: `catch (...) { (void)casGcMaintenanceState(backend, layout, progress.token, GcMaintenanceState{}); throw; }`.
  `GcMaintenanceState{}` — это пустой `janitor_cursor` (`Formats/CasGcMaintenanceStateFormat.h:14`), т.е. именно откат к началу префикса.
  Исключение при этом улетает наружу, но у вызывающего оно только логируется: `Gc/CasGc.cpp:485-488`
  (`LOG_WARNING(... "CAS namespace janitor skipped this round: {}")`), т.е. раунд продолжается, а курсор уже стёрт.
- Пейсинг тоже подтверждён: `Gc/CasGc.cpp:470` создаёт `NamespaceJanitor janitor(backend, layout, 1000)`, и `runNamespaceJanitorPage`
  вызывается ровно один раз за раунд (`Gc/CasGc.cpp:710` — suppressed-путь, `Gc/CasGc.cpp:1221` — обычный). Так что «одна страница в 1000 ключей
  на раунд» — факт, и откат курсора действительно стоит всего накопленного прогресса.
- Ключевая проверка, которую аудит не сделал, но которая играет в его пользу: курсор — НЕ непрозрачный истекающий continuation token, а последний
  выданный ключ, возобновление через `start_after` (`Backend/CasBackend.h:124-129`: «cursor resumes strictly after the last returned key»,
  `:129` «`next_cursor` — Last returned key», `:260-262`; реализация — `Backend/CasObjectStorageBackend.cpp:1177-1185`, комментарий прямо говорит
  «The backend cursor is "last key returned" (exclusive on resume)»). Значит транзиентный 5xx/throttle/timeout курсор не портит, и сброс для
  восстановления не нужен — сброс здесь безусловный и потому чрезмерный.
- Поведение преднамеренное и запинено тестом: `gtest_cas_namespace_janitor.cpp:548-561`
  `CASNamespaceJanitor.BackendRejectedCursorResetsExactlyAndDeletesNothing` с бэкендом `RejectCursorBackend` (`:140-149`), который швыряет
  «backend rejected cursor» на любой непустой курсор; тест ожидает `EXPECT_TRUE(readGcMaintenanceState(...).state->janitor_cursor.empty())`.
  То есть замысел — вылезти из «отравленного» курсора, но код не различает отказ по курсору и обычный транзиентный сбой. Введено сразу при
  рождении уборщика: `git log -S 'casGcMaintenanceState(backend, layout, progress.token, GcMaintenanceState{})'` → единственный коммит
  `111bb12a407` «Add perpetual namespace janitor» (2026-08-02); поздних изменений этого блока нет, находка НЕ устарела.

Что в утверждениях аудита неточно/преувеличено:
- «one S3 5xx … discards all prior progress» — верно; но «can make no net progress at all» требует, чтобы вероятность отказа LIST была
  сравнима с темпом продвижения страниц. У S3-бэкенда LIST идёт через обычный клиент ClickHouse с его retry-стратегией, так что до уровня
  уборщика доходят в основном не-транзиентные или упорные (throttling-шторм) отказы. Как «может» — корректно, как «типично» — нет.
- Класс «LEAK» — только в смысле отложенного реклейма. Тихой порчи нет: удаления делаются exact-token (`Gc/CasNamespaceJanitor.cpp:111`),
  повторный проход по уже пройденным ключам идемпотентен; сброс курсора — best-effort CAS под `progress.token`, проигрыш CAS просто игнорируется.
  Провал громкий и посчитанный: аномалии логируются (`Gc/CasGc.cpp:481-482`), остаток виден в fsck как
  `namespace_janitor_pending`/`_bytes`/`_lives` (`Tools/CasFsck.h:144-157`).
- Плюс уже устоявшаяся позиция: латентность стирания не входит в контракт диска (оператор может дать `GC RUN` в любой момент) — зафиксировано в
  `docs/superpowers/cas/BACKLOG/gc.md`{#janitor-page-hardcoded} (2031-triage CAS-034). Это ограничивает серьёзность именно этим классом.

BACKLOG: существующего пункта именно про сброс курсора не было (грепы по `janitor` в `BACKLOG.md` и `BACKLOG/*.md` дают только
{#janitor-page-hardcoded}, `[gc-files-prefix-not-listed]` и упоминание в {#rebuild-gcstate-decode-reason-unreported}). Остаточек реальный и
неотслеженный, поэтому добавлен новый (незакоммиченный) раздел
`docs/superpowers/cas/BACKLOG/gc.md`{#janitor-cursor-rewind-on-list-error} с формулировкой долга: не сбрасывать курсор на транзиентном сбое —
сбрасывать только на детерминированном отказе по курсору (класс invalid-argument) либо после N подряд неудач на одном и том же курсоре;
тест `BackendRejectedCursorResetsExactlyAndDeletesNothing` при этом надо перецелить на «детерминированный отказ», а не «любое исключение».

## CAS-079 — Ревалидация ref-cleanup в GC действительно требует неподвижности токена всего пул-глобального каталога и при отказе выходит из всей фазы — живой остаток того самого класса, который для ref-writer уже убран коммитом 684161dcc03. (подтверждено, P2) {#cas-079}

Оба утверждения аудита подтверждены на HEAD (файл переехал в `Gc/CasGc.cpp`, символ — `Gc::cleanupRefObjects`, начало `Gc/CasGc.cpp:3374`):

1. Ревалидация по токену ВСЕГО каталога: внутри лямбды `deleteRefObject` перед каждым `deleteExact` делается свежий
   `CasRefCatalog::read` (`Gc/CasGc.cpp:3417`), и первым же дисъюнктом отказа стоит
   `if (current_catalog.token != folded.catalog_cut->token` (`Gc/CasGc.cpp:3424`) — при несовпадении `LOG_DEBUG` «catalog observation/life moved»
   и `return false` (`:3429-3433`). При этом остальные дисъюнкты того же условия уже проверяют СТРОКУ этого namespace по значению и
   переразрешают его life: `current_entry_it->ns != ns || *current_entry_it != observed_entry || !current_life || *current_life != life`
   (`:3425-3428`), а неоднозначность life-индекса проверяется отдельно (`:3418` `throwIfAmbiguous("CAS GC ref cleanup revalidation")`).
   То есть сравнение токена — строго избыточное усиление: оно отказывает и тогда, когда изменилась чужая строка каталога.
2. Каталог — один объект на весь пул: `layout.refCatalogKey()` — единственный ключ, это же зафиксировано измерением в
   `docs/superpowers/cas/BACKLOG/performance.md`{#ref-catalog-write-hotspot} («every table creation in a pool writes the same catalog object
   `cas/ref_catalog`»). Мутации каталога происходят на событиях жизненного цикла namespace (создание при первой записи —
   `precommitAdd` → `appendRefOps` → `createNamespace`, см. `BACKLOG/gc.md:155`; DROP; переходы Creating→Live→Removing), а не на каждый INSERT.
3. Выход из ВСЕЙ функции при первом отказе: `if (!deleteRefObject(layout.refLogKey(life, log_id))) return;` (`Gc/CasGc.cpp:3506`) и
   `if (!deleteRefObject(layout.refSnapshotKey(life, snap_id))) return;` (`Gc/CasGc.cpp:3518`) — оба внутри цикла `for (const auto & [ns_str, listing] : folded.ref_tables)`
   (`Gc/CasGc.cpp:3388`), так что отказ на namespace A обрывает подрезку и всех оставшихся namespace B..Z этого раунда. Комментарий это и декларирует:
   «A moved token, changed row/life, … stops the whole cleanup pass» (`:3405-3409`).
4. Окно — длинное: `folded.catalog_cut` берётся во время fold, а cleanup идёт уже после единственного `gc/state` CAS раунда (см. комментарий
   теста `gtest_cas_ref_gc.cpp:490-491` «The SAME round that folds the whole tail also runs post-CAS cleanup»), т.е. окно накрывает весь
   O(pool)-fold. На занятом кластере с созданием/удалением таблиц отказ действительно вероятен в большинстве раундов — как аудит и говорит.

Это ЖИВОЙ остаток известного класса, а не новая гипотеза. HEAD-коммит `684161dcc03` «cas: prove namespace absence per-row, not by whole-catalog
stillness» убрал ровно такое условие из двух мест `Pool/CasRefLedger.cpp` (presence probe и cold-reader admission), с измеренным симптомом:
`01069_database_memory` падал 193 из 194 ретраев на «catalog changed while probing table-root cleanup completeness». GC-шный сайт в тот коммит не
попал (`git show --stat 684161dcc03` — только `Pool/CasRefLedger.cpp/.h` и `gtest_cas_ref_writer.cpp`). Более того, старый контракт здесь ЗАПИНЕН
тестом `CASRefGcCleanupAuthority.CatalogTokenMoveBeforeFirstDeleteRefusesEveryRefObjectDelete` (`gtest_cas_ref_gc.cpp:563-579`), и инъекция
двигает токен, перезаписывая каталог БАЙТ-В-БАЙТ тем же содержимым (`RefCleanupAuthorityRaceBackend::moveAuthority`, `gtest_cas_ref_gc.cpp:159-176`:
для `Authority::Catalog` `bytes = got->bytes` без изменений) — то есть тест пинит именно чрезмерную чувствительность, а парный тест
`CatalogTokenMoveBetweenKeysAllowsFirstAndRefusesSecondDelete` (`gtest_cas_ref_gc.cpp:582-599`) пинит обрыв «после первого удаления».

Границы серьёзности (почему P2, а не P1): это только латентность/накопление ref-объектов, не потеря и не тихая порча. Направление отказа
fail-closed: ни одного удаления без подтверждённой авторизации, и кандидаты пересчитываются из durable-состояния в следующем раунде — комментарий
`Gc/CasGc.cpp:3497-3501` («`planRefCleanup` recomputes the SAME remaining candidates from durable state next round, so nothing here needs its own
cursor»). Практический вред: неограниченный рост `_log`/`_snap` под lifecycle-нагрузкой (стоимость хранения, длиннее LIST, длиннее реплей при
восстановлении) — тот же симптом starvation, который уже наблюдался в CI на аналогичном месте.

BACKLOG: пункта на это не было (грепы `catalog token`/`stillness`/`ref cleanup`/`whole-catalog` по `BACKLOG.md` и `BACKLOG/*.md` — пусто; ближайший
сосед — `BACKLOG/docs-and-cleanup.md:36-38` «The destructive gate collapses per-namespace facts into a pool-wide boolean», другой механизм).
Добавлен новый (незакоммиченный) раздел `docs/superpowers/cas/BACKLOG/gc.md`{#ref-cleanup-whole-catalog-token-stillness}. Owed: (а) ревалидация
по СТРОКЕ, как в `684161dcc03` — строка по значению + переразрешение life + явный `throwIfAmbiguous` (именно он ловит aliasing-инкарнацию, которую
раньше ловило сравнение токена); (б) отказ ограничить своим namespace (`continue`), а не всей фазой; (в) перецелить оба пинящих теста на
per-row-контракт, добавив регресс на «чужая мутация каталога не должна останавливать подрезку».
