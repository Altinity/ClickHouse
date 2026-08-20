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
| CAS-007 | ⏳ | — | — | — | — |
| CAS-008 | ⏳ | — | — | — | — |
| CAS-009 | ⏳ | — | — | — | — |
| CAS-010 | ⏳ | — | — | — | — |
| CAS-011 | ⏳ | — | — | — | — |
| CAS-012 | ⏳ | — | — | — | — |
| CAS-013 | ⏳ | — | — | — | — |
| CAS-014 | ⏳ | — | — | — | — |
| CAS-015 | ⏳ | — | — | — | — |
| CAS-016 | ⏳ | — | — | — | — |
| CAS-017 | ⏳ | — | — | — | — |
| CAS-018 | ⏳ | — | — | — | — |
| CAS-019 | ⏳ | — | — | — | — |
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
