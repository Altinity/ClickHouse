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
| CAS-016 | ⏳ | — | — | — | — |
| CAS-017 | ⏳ | — | — | — | — |
| CAS-018 | частично | P3 | [{#noexcept-allocation-hardening}](BACKLOG/ref-protocol.md#noexcept-allocation-hardening) | нет | Головной механизм (утечка лидерства в ref-очереди) уже закрыт единой точкой выхода и тестами; из шести якорей подтверждаются только теоретические аллокации в `noexcept`/деструкторах под лимитом памяти, а «renewal фенсит маунт» — прямо неверно. |
| CAS-019 | частично | P2 | [{#part-folder-single-flight-manifest-keying}](BACKLOG/ref-protocol.md#part-folder-single-flight-manifest-keying) | нет | Ключ single-flight действительно только `ns+ref` и post-wait проверки manifest id нет, но каждый выданный view внутренне консистентен (один манифест), сингл-флайт работает только на stale-терпимом `CachedForLoad`, так что последствие — сдвиг на один репойнт, а не смешение двух манифестов. |
| CAS-020 | ⏳ | — | — | — | — |
| CAS-021 | ⏳ | — | — | — | — |
| CAS-022 | ⏳ | — | — | — | — |
| CAS-023 | ⏳ | — | — | — | — |
| CAS-024 | ⏳ | — | — | — | — |
| CAS-025 | ⏳ | — | — | — | — |
| CAS-026 | ⏳ | — | — | — | — |
| CAS-027 | ⏳ | — | — | — | — |
| CAS-028 | ⏳ | — | — | — | — |
| CAS-029 | ⏳ | — | — | — | — |
| CAS-030 | ⏳ | — | — | — | — |
| CAS-031 | ⏳ | — | — | — | — |
| CAS-032 | ⏳ | — | — | — | — |
| CAS-033 | ⏳ | — | — | — | — |
| CAS-034 | ⏳ | — | — | — | — |
| CAS-035 | ⏳ | — | — | — | — |
| CAS-036 | ⏳ | — | — | — | — |
| CAS-037 | ⏳ | — | — | — | — |
| CAS-038 | ⏳ | — | — | — | — |
| CAS-039 | ⏳ | — | — | — | — |
| CAS-040 | ⏳ | — | — | — | — |
| CAS-041 | ⏳ | — | — | — | — |
| CAS-042 | ⏳ | — | — | — | — |
| CAS-043 | ⏳ | — | — | — | — |
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
