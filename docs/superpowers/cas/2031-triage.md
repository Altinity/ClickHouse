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
| CAS-036 | ⏳ | — | — | — | — |
| CAS-037 | ⏳ | — | — | — | — |
| CAS-038 | ⏳ | — | — | — | — |
| CAS-039 | ⏳ | — | — | — | — |
| CAS-040 | ⏳ | — | — | — | — |
| CAS-041 | ⏳ | — | — | — | — |
| CAS-042 | by-design | P2 | [{#operability} (B180 / format-freeze), {#cas-format-version-floor}](BACKLOG.md#operability} (B180 / format-freeze), {#cas-format-version-floor) | нет | Форма кода описана верно (одна глобальная генерация как min-reader, `changePoints` не читается на декоде), но это осознанная pre-release политика recreate-only; следствия про «тихое стирание полей» и про `Roster` недостижимы. |
| CAS-043 | частично | P3 | [{#relink-fallback-unknown-format-version}](BACKLOG.md#relink-fallback-unknown-format-version) | нет | Узость catch подтверждена (`CORRUPTED_DATA` only, а гейт версии и критический ключ дают `UNKNOWN_FORMAT_VERSION`), но перекос генераций в одном пуле сегодня невозможен: relink предлагается только внутри одного смонтированного пула, а mount держит точный гейт генерации — остаётся однострочное упрочнение. |
| CAS-044 | ⏳ | — | — | — | — |
| CAS-045 | ⏳ | — | — | — | — |
| CAS-046 | ⏳ | — | — | — | — |
| CAS-047 | ⏳ | — | — | — | — |
| CAS-048 | ⏳ | — | — | — | — |
| CAS-049 | ⏳ | — | — | — | — |
| CAS-050 | ⏳ | — | — | — | — |
| CAS-051 | ⏳ | — | — | — | — |
| CAS-052 | ⏳ | — | — | — | — |
| CAS-053 | ⏳ | — | — | — | — |
| CAS-054 | ⏳ | — | — | — | — |
| CAS-055 | ⏳ | — | — | — | — |
| CAS-056 | ⏳ | — | — | — | — |
| CAS-057 | ⏳ | — | — | — | — |
| CAS-058 | ⏳ | — | — | — | — |
| CAS-059 | ⏳ | — | — | — | — |
| CAS-060 | ⏳ | — | — | — | — |
| CAS-061 | ⏳ | — | — | — | — |
| CAS-062 | ⏳ | — | — | — | — |
| CAS-063 | ⏳ | — | — | — | — |
| CAS-064 | ⏳ | — | — | — | — |
| CAS-065 | ⏳ | — | — | — | — |
| CAS-066 | ⏳ | — | — | — | — |
| CAS-067 | ⏳ | — | — | — | — |
| CAS-068 | ⏳ | — | — | — | — |
| CAS-069 | ⏳ | — | — | — | — |
| CAS-070 | ⏳ | — | — | — | — |
| CAS-071 | ⏳ | — | — | — | — |
| CAS-072 | ⏳ | — | — | — | — |
| CAS-073 | ⏳ | — | — | — | — |
| CAS-074 | ⏳ | — | — | — | — |
| CAS-075 | ⏳ | — | — | — | — |
| CAS-076 | ⏳ | — | — | — | — |
| CAS-077 | ⏳ | — | — | — | — |
| CAS-078 | ⏳ | — | — | — | — |
| CAS-079 | ⏳ | — | — | — | — |
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
