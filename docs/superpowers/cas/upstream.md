Optional parallel track (serves the fork goal directly)

G. Carve generic fixes into separate upstream PRs to shrink the fork's Ring-2 diff: ThreadStatus B90, ReadBufferFromFileView B115, ReadBufferFromS3 B117, LocalObjectStorage TOCTOU, MergeTreeDeduplicationLog null-writer, copyS3File message_format_string, Expect: 100-continue, S3Exception::isPreconditionFailed(), GCS dialect/signer, clickhouse-disks non-interactive exit code (see below). Not blocking; noted because it's the cleanup that most reduces future conflicts.


G-item: `clickhouse-disks --query` returns the failing command's error code
--------------------------------------------------------------------------

**Site.** `DisksApp::main` in `programs/disks/DisksApp.cpp`. `processQueryText` records each command's
error code in `last_command_exit_code`; `main` returns it as the process exit code, guarded by
`query.has_value()` so interactive REPL sessions keep exiting 0. Before this, `clickhouse-disks --query
<cmd>` always exited 0 and reported failures only on stderr and in the log. Error printing is unchanged.

**Why it is right.** A non-interactive invocation is by definition something a script, a cron job or a
CI step is driving, and a tool that always exits 0 cannot be gated on. The CAS applets are how it was
noticed — `cas-fsck` signals INV-NO-LOSS violations by throwing, and both the ca-soak harness
(`utils/ca-soak/soak/fsck.py`) and the CAS integration tests gate on that exit code — but the argument
is entirely generic and the hole belongs to every user of the tool.

**It changes behavior for every `clickhouse-disks` user**, not only CAS ones, including out-of-tree
scripts that today rely on the swallowing. Two details a reviewer will want. Within one
semicolon-separated `--query` batch the code is reset once at entry and each failure overwrites it, so
a later success does NOT clear an earlier failure: the exit code means "something in this batch
failed", not "the last command failed". And `remove` throws on an absent path
(`CommandRemove::executeImpl`) while `list` does not (`DiskWithPath::listAllFilesByPath` returns an
empty vector for a non-directory), so `remove` is the verb most likely to newly fail a caller.

**It exposed a latent test defect, which the carve-out must carry.**
`tests/integration/test_replicated_database/test.py::test_replicated_table_structure_alter` read
`metadata_path` for `table_structure.mem` out of `system.tables` on `competing_node` AFTER that node
had already run `DETACH DATABASE table_structure`. A detached database has no rows in `system.tables`,
so the SELECT returned the empty string and the `--query "remove "` that followed failed on a missing
mandatory `path` argument. The metadata file the test meant to delete was never deleted, and the test
passed anyway because the exit code was swallowed — so the corruption-recovery scenario it claims to
exercise had not been exercised. Fixed by reading the path before the DETACH and asserting it is
non-empty. That file is upstream's and otherwise unmodified by us.

**Blast radius (swept 2026-08-03).** Every `clickhouse-disks` / `clickhouse disks` invocation with
`--query`/`-q` in `tests/`, `ci/` and `utils/` was enumerated and classified: the sweep found exactly
one victim, the one above. Stateless `.sh` tests are structurally insulated — none set `-e` or
`pipefail`, so only a script's last command decides its status, and in every disks-using stateless test
that last command is either a `CLICKHOUSE_CLIENT` call or a `remove` of a path an immediately preceding
assertion proved present. Integration tests are the exposed surface, because `exec_in_container`
defaults to `nothrow=False`. The CAS integration tests and the ca-soak harness already read the exit
code deliberately; `ci/jobs/scripts/clickhouse_proc.py` validates the output it gets
(`is_valid_uuid`) rather than trusting the exit code. Detail:
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/disks-exitcode-report.md`.

**Carve-out obligation.** The change RIDES IN the CAS pull request for now — the feature is
pre-release and the CI gating it enables is needed there — but it must LATER be carved out and travel
as its own upstream PR, carrying this rationale plus the test fixes it forces. It must NOT be silently
bundled into a CAS-feature PR when the series is produced: it is a behavior change to a shared tool,
and a reviewer who cares about `clickhouse-disks` would never see it there. Shape-wise it belongs to
Workstream A1 (standalone fixes) of the merge-layout spec.


 Ниже формулировка, которую я бы использовал в issue/PR description.

  Проблема
  В MergeTree уже был механизм дисковых транзакций (IDiskTransaction), но часть кода фактически использовала его как “batch of file operations”, а не как транзакцию создания одного логического part-а. Из-за этого в путях с projections, clone/freeze и mutable per-part files один logical part мог строиться через несколько независимых disk transactions или через autocommit
  операций по отдельным файлам.

  Для текущего non-CAS ClickHouse это обычно не приводило к прямой потере данных: на local disk операции видны сразу, а на обычном object-storage metadata storage part всё ещё представлен набором независимых файлов. Но следствие всё равно неприятное: общий контракт был слишком слабым. Код MergeTree полагался на ранние commit подпартов/projections, чтобы потом прочитать их
  обратно, вместо того чтобы держать один transaction и читать staged состояние из него. Это делает атомарность part-а зависимой от реализации диска и оставляет больше окон для orphan temp metadata/blobs после исключений.

  Конкретные симптомы

  1. Projection sub-part мог открывать и коммитить свою транзакцию отдельно от parent part.
     Фиксы:
      - 24f89d7ce78 — route merge/mutate projection sub-part through parent whole-part transaction.
      - 73b03c9a1e4 — encapsulate whole-part-transaction rule on borrowed projection storage.
      - src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:41 передаёт parent transaction в projection storage.
      - src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:347 и src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:363: borrowed transaction делает beginTransaction/commitTransaction no-op.
      - src/Storages/MergeTree/MergeTask.cpp:561, src/Storages/MergeTree/MergeTask.cpp:1319, src/Storages/MergeTree/MutateTask.cpp:1815.

  2. Код не имел нормального read-your-writes API для файлов, staged внутри disk transaction. Поэтому подпарт приходилось коммитить рано, чтобы последующий код мог его увидеть.
     Фиксы:
      - ec4b75eeb35 — forward in-flight resolve through DiskObjectStorageTransaction.
      - 80d4e0272eb — DataPartStorageOnDiskFull consults its build transaction for in-flight reads.
      - 828040aa662, 51192075c3f, f528a7f2b7e, 998c67f0b46 — добивают file/dir/listing cases.
      - src/Disks/IDiskTransaction.h:142 добавляет default no-op tryGetInFlightStorageObjects, tryReadFileInFlight, tryGetInFlightFileSize, hasInFlightDirectory, listInFlightDirectory.
      - src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:51, src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:128, src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:203.

  3. Clone/freeze path мог работать per-file, без enclosing transaction для whole part.
     Для non-CAS это исторически допустимо, но это и есть место, где abstraction leak виден сильнее всего: логический clone part-а не выражен как одна disk transaction.
     Фиксы:
      - 8fcea70ae3d и связанные clone/un-gate fixes.
      - src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:514 создаёт whole-part transaction, если caller её не дал.
      - src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:553 пишет metadata_version.txt внутрь clone transaction.

  4. Порядок staged операций внутри transaction был недостаточно явно задан. Для CAS пришлось eager-stage createHardLink, mutable moveFile/replaceFile, moveDirectory, иначе часть MergeTree-кода читала состояние до commit и видела старую картину.
     Фиксы:
      - 706329d441d, 2027de8346a, 6f5e3866710.
      - src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:151, src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:170, src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:195, src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:545.

  Следствие для non-CAS
  Я бы формулировал осторожно: это не обязательно user-visible bug для существующих дисков, а design debt в общем слое. Текущие реализации выживали за счёт ранних commits и immediate filesystem visibility. Но такой контракт плохо масштабируется на любые диски, где part должен публиковаться как один объект или где staged metadata не видна до commit. Поэтому фиксы полезны не
  только CAS: они уточняют границу ответственности DataPartStorage/IDiskTransaction и убирают неявную зависимость MergeTree от “каждый подпарт можно безопасно коммитить отдельно”.


=================


• В текущей git-истории первые disk transactions привнёс alesapin.

  Первый коммит:

  - b557c1dd80db от 2022-06-15 13:51 +0200
  - Author/Committer: alesapin <alesapin@gmail.com>
  - subject: Interm stage
  - добавил:
      - src/Disks/IDiskTransaction.h
      - src/Disks/FakeDiskTransaction.h
      - src/Disks/ObjectStorages/DiskObjectStorageTransaction.cpp
      - начал подключение к MergeTree через src/Storages/MergeTree/IMergeTreeDataPart.cpp

  То есть интерфейс IDiskTransaction, fake implementation для обычных дисков и transaction implementation для object-storage дисков появились вместе.

  Дальше эта же серия быстро допиливалась:

  - 11b6664863b от 2022-06-16, alesapin, WI{
    Существенно расширил DiskObjectStorageTransaction.

  - b11439ff36e от 2022-06-16, alesapin, Compiles
    Привёл реализацию к компилируемому состоянию.

  - ae8108d30b9 от 2022-06-19, alesapin, Add comments, better interface
    Уже ближе к оформленному интерфейсу.

  - 7ae775d2db4, 6429b723718, 0d3904b788d, 9a891543524 от 2022-06-20..30, alesapin
    Review/fixes, включая Fix incorrect order of operations in disk transactions.

  Позже важные доработки:

  - 6c495863667 от 2024-03-27, Azat Khuzhin
    Lock-free parts rename for ReplicatedMergeTree to avoid INSERT affect SELECT
    Это про rename_in_transaction.

  - 3675c27fe9e от 2024-04-04, Azat Khuzhin
    Require explicit rename of parts in transaction.

  - 1f3685a81f3 от 2025-07-10, Sema Checherinda
    read from own trunsaction to check uncommited part.

  Так что коротко: первично disk transactions появились 15 июня 2022, автор alesapin; затем в течение второй половины июня 2022 эта серия была доведена до рабочего состояния.


======================


• Контекст старых изменений такой:

  - 2022, alesapin: IDiskTransaction появился как batch layer для object-storage metadata, где нельзя просто делать filesystem rename/hardlink. Уже тогда была проблема порядка операций: 9a891543524 прямо называется Fix incorrect order of operations in disk transactions.
  - 2024, Azat: rename_in_transaction использовали для lock-free rename в ReplicatedMergeTree, чтобы не держать DataPartsLock во время дорогого rename. Там же видно, что обычный MergeTree оставили осторожно: “not correct for now”.
  - 2025, Sema: 1f3685a81f3 добавил “read from own transaction”, потому что transactional disk должен видеть свои uncommitted writes; иначе код вынужден коммитить текущую транзакцию и начинать новую.

  На этом фоне наши фиксы вне CAS улучшают не “CAS-поведение”, а общий контракт дисковых транзакций в MergeTree.

  Что именно улучшается

  1. DataPartStorage становится владельцем правила “кто владеет транзакцией”
     Раньше call site должен был знать: этот projection sub-part открывает свою транзакцию или едет в parent transaction. Это легко ломается: один part начинает собираться несколькими транзакциями.

  Наш фикс:

  - 73b03c9a1e4 cas: encapsulate whole-part-transaction rule on borrowed projection storage
  - src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:347: beginTransaction no-op для borrowed transaction.
  - src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:363: commitTransaction no-op для borrowed transaction.

  Для non-CAS это полезно как API cleanup: call sites больше не должны размазывать условия вида “если storage особый, не начинай транзакцию”. Это снижает риск повторить старую ошибку с несколькими транзакциями на один logical part.

  2. read-your-writes становится частью транзакционного контракта, а не ad hoc проверкой
     Старый фикс 1f3685a81f3 уже признал проблему: настоящая транзакция должна видеть свои незакоммиченные записи. Но API был грубее: listUncommittedDirectoryInTransaction, readUncommittedFileInTransaction, validateTransaction, isTransactional.

  Наши фиксы делают это точнее:

  - ec4b75eeb35 — forward in-flight resolve through DiskObjectStorageTransaction.
  - 80d4e0272eb — DataPartStorageOnDiskFull consults its build transaction for in-flight reads.
  - 51192075c3f — directory-granularity visibility.
  - src/Disks/IDiskTransaction.h:142: per-path tryGetInFlightStorageObjects, tryReadFileInFlight, tryGetInFlightFileSize, hasInFlightDirectory, listInFlightDirectory.
  - src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:51, src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:128, src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp:203.

  Для non-CAS это означает: future/real transactional disks получают нормальный способ читать staged state без premature commit/reopen. Для local fake transaction поведение почти не меняется: defaults пустые, всё и так видно immediately.

  3. Projection build/merge/mutation перестаёт требовать специальных знаний о диске
     Ветка переводит call sites к нормальному виду: всегда вызвать beginTransaction/commitTransaction, а storage сам решает, owns ли он transaction.

  Файлы:

  - src/Storages/MergeTree/MergeTask.cpp:561
  - src/Storages/MergeTree/MergeTask.cpp:1319
  - src/Storages/MergeTree/MutateTask.cpp:1815
  - src/Storages/MergeTree/MergeTreeDataWriter.cpp:1032

  Для non-CAS это снижает coupling. Старые transaction changes уже пытались использовать disk transaction как abstraction boundary; наши фиксы делают эту границу менее дырявой.

  4. Clone/freeze получает более правильную модель: clone part-а как одна операция

  - src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:514
  - src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:553

  Для обычных дисков это почти не нужно, но для object-storage-like дисков это правильная семантика: clone logical part-а не должен превращаться в набор независимых autocommit file operations.

  Итого: вне CAS наши фиксы улучшают не текущий local disk happy path, а общий слой MergeTree ↔ IDiskTransaction: меньше преждевременных commit, меньше специальных веток в call sites, более явный ownership транзакции, и нормальный read-your-writes контракт для настоящих transactional disks. CAS просто стал первым потребителем, на котором старые допущения перестали быть
  безвредными.


======


ThreadStatus B90
  Проблема: borrowed ThreadGroup может пережить parent/group counters, если async/background work захватывает query thread group и выполняется после окончания запроса. Следствие: use-after-free в parent counters или тонкие memory tracking повреждения.

  Фиксы/коммиты:
  e3fb8393b9b, затем серия 5f91e4cbdb1, 32ee5704471, 4b67323eb74; upstream-like фиксы есть как 7b5c27672c0/f74d935943b.

  Как увидеть upstream:

  - grep: git grep -n borrowed_thread_group upstream/master -- src/Common src/Storages
  - тестовый признак: нужен gtest вроде gtest_borrowed_thread_group_lifetime.cpp, который создаёт borrowed child group, уничтожает parent, потом трогает child counters под ASan/Debug.
  - runtime-признак: ASan UAF около ThreadStatus/memory tracker после окончания query, если callback-runner или background write держит query group.


================


  ReadBufferFromFileView B115
  Проблема: ReadBufferFromFileView некорректно обновлял file_offset_of_buffer_end, предполагая, что inner buffer не сбрасывает рабочий буфер при setReadUntilPosition. ReadBufferFromS3 как раз сбрасывает. Итог: getPosition начинает врать, seek/mark logic может перечитать старый decompressed block.

  Фикс:
  440871098a9
  Файлы: src/IO/ReadBufferFromFileView.cpp, src/IO/tests/gtest_read_buffer_from_file_view.cpp.

  Как увидеть upstream:

  - grep: git show upstream/master:src/IO/ReadBufferFromFileView.cpp
  - если после impl->setReadUntilPosition нет пересчёта impl->getPosition() + impl->available(), баг есть.
  - минимальный тест: inner buffer, который на setReadUntilPosition делает file_offset = getPosition(); resetWorkingBuffer();, потом view читает с right-bound посередине текущего буфера. В форке добавленный gtest ловил 14/36 failing cases.
  - user-visible: packed files / PackedFilesReader поверх remote-like buffer, optimize_read_in_order, wrong rows без checksum error.

======================

  ReadBufferFromS3 B117
  Проблема: SDK retry strategy уже останавливается на query cancellation, но внешний retry loop в ReadBufferFromS3::processException продолжал ретраить transient ошибки после KILL QUERY.

  Фикс:
  dd408fef7ba
  Файл: src/IO/ReadBufferFromS3.cpp.

  Как увидеть upstream:

  - grep: git grep -n "processException" upstream/master -- src/IO/ReadBufferFromS3.cpp
  - в processException должен быть CurrentThread::get().isQueryCanceled().
  - runtime: начать S3 read, оборвать соединение/вызвать transient error, затем KILL QUERY; без фикса thread живёт до s3_max_single_read_retries с backoff.


=====================

  LocalObjectStorage TOCTOU
  Проблема: listObjects получает entry из directory iterator, потом делает file_size/metadata stat. Если файл удалён между listing и stat, local object storage бросает filesystem_error. Реальный object store в такой ситуации просто не вернул бы исчезнувший объект.

  Фиксы:
  в нашей ветке 85aa0ed6908; upstream уже имеет похожие фиксы от Groene AI: 4bce1b998ad, 82899b24723, 88a9f334f1a, dbf333d46c3, merge 89a3174a3a6.

  Как увидеть upstream:

  - grep: git grep -n "tryGetObjectMetadata(entry" upstream/master -- src/Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.cpp
  - если listObjects вызывает throwing getObjectMetadata, баг есть; если tryGetObjectMetadata и skip vanished entries, уже исправлено.
  - repro: один поток рекурсивно list-ит local object storage prefix, второй удаляет файлы в этом prefix; без фикса ловится No such file or directory.

==============================

  MergeTreeDeduplicationLog null-writer
  Проблема: MergeTreeDeduplicationLog::addPart/dropPart проверяли current_writer только через chassert; в release это no-op. Если writer не создан, дальше dereference null pointer.

  Фикс:
  39e3a14f546
  Файл: src/Storages/MergeTree/MergeTreeDeduplicationLog.cpp.

  Как увидеть upstream:

  - grep: git grep -n "chassert(current_writer" upstream/master -- src/Storages/MergeTree/MergeTreeDeduplicationLog.cpp
  - если нет обычного if (!current_writer) throw Exception(...), release может получить segfault.
  - repro: table with non_replicated_deduplication_window > 0 on disk/path where append log writer is not available/failed; call addPart.

=========================

  copyS3File message_format_string
  Проблема: copyS3File.cpp делал fmt::format(...), потом передавал готовую строку в S3Exception. Это теряет static message_format_string, поэтому system.text_log/system.errors хуже группируют ошибки.

  Фикс:
  396e5dc5e94
  Файл: src/IO/S3/copyS3File.cpp.

  Как увидеть upstream:

  - grep: git grep -n "throw S3Exception(.*fmt::format" upstream/master -- src/IO/S3/copyS3File.cpp
  - правильный вариант: S3Exception(PreformattedMessage::create("Message: {}, Key: ...", ...), ...).
  - runtime: вызвать failing multipart/copy path, затем смотреть system.text_log.message_format_string; без фикса там будет уже форматированный текст или пустой grouping key.

=========================

  Expect: 100-continue
  Проблема: conditional PUT с большим body и If-None-Match/If-Match может быть заранее обречён на 412. Без Expect: 100-continue ClickHouse всё равно стримит body; некоторые S3-compatible stores закрывают соединение mid-upload или дают retryable 500. Итог: retry storm и зависание merge/insert.

  Фиксы:
  5b69a99fd5b, scoped by 55b4b3e580f, настройка threshold 8948d309ad2.
  Файлы: src/IO/S3/PocoHTTPClient.cpp, PocoHTTPClient.h, S3AuthSettings.cpp, S3Defines.h.

  Как увидеть upstream:

  - grep: git grep -n "setExpectContinue\\|peekResponse\\|expect_continue_min_bytes" upstream/master -- src/IO/S3
  - если conditional PUT не вызывает setExpectContinue and peekResponse, проблема есть.
  - repro: S3-compatible store like RustFS, conditional PUT If-None-Match: * на уже существующий object с body сотни KB/MB. Без фикса видны Broken pipe/500 and many retries.

  S3Exception::isPreconditionFailed / one 412 policy
  Проблема: HTTP 412 Precondition Failed deterministic для conditional request. Retrying never helps. AWS SDK может классифицировать non-AWS error body как retryable UNKNOWN; разные места проверяли либо status, либо ExceptionName, либо substring.

  Фиксы:
  b566f00ef4f, generalized by ef9931bfd3a.
  Файлы: src/IO/S3/Client.cpp, src/IO/S3Common.h/.cpp, src/IO/WriteBufferFromS3.cpp.

  Как увидеть upstream:

  - grep: git grep -n "PRECONDITION_FAILED\\|isPreconditionFailedError\\|isPreconditionFailed" upstream/master -- src/IO/S3 src/IO/S3Common*
  - если retry strategy не returns false on PRECONDITION_FAILED, bug.
  - unit: create AWSError(CoreErrors::UNKNOWN, retryable=true), set response code PRECONDITION_FAILED; ShouldRetry must be false.
  - runtime: RustFS/non-AWS 412 body produces many retries instead of immediate conditional-failure handling.

====================

  GCS dialect/signer
  Проблема: GCS XML API is S3-like, but not S3. For generation-safe conditional writes it needs native GOOG4 signing and x-goog-if-generation-match; AWS If-Match/If-None-Match/SigV4 semantics are not enough. Worse, conditional multipart complete can be silently ignored by GCS.

  Фиксы:
  8138a8313f2 GOOG4Signer, 41a247e3310 conditional dialect, 9604d6a5be9 Poco client mode integration.
  Файлы: src/IO/S3/GOOG4Signer.*, src/IO/S3/GCSConditionalDialect.*, src/IO/S3/PocoHTTPClient.*, tests.

  Как увидеть upstream:

  - grep: git grep -n "GOOG4\\|x-goog-if-generation-match\\|GCSConditionalDialect" upstream/master -- src/IO/S3
  - if absent, upstream S3-compatible path cannot safely express GCS generation preconditions through existing S3 client.
  - tests: fixed GOOG4 vectors; dialect tests:
      - If-None-Match: * -> x-goog-if-generation-match: 0
      - numeric If-Match -> x-goog-if-generation-match: <generation>
      - non-numeric If-Match throws
      - conditional CompleteMultipartUpload throws.

  - live repro: GCS bucket, generation match conditional write through S3-compatible path. Without dialect/signer, request is rejected, mis-signed, or has wrong conditional semantics.

  Practical PR split:

  1. ReadBufferFromFileView + gtest.
  2. ReadBufferFromS3 cancellation.
  3. MergeTreeDeduplicationLog null-writer guard.
  4. copyS3File PreformattedMessage.
  5. S3 412 no-retry + helper.
  6. Expect: 100-continue threshold.
  7. GCS GOOG4/dialect as a feature PR.
  8. LocalObjectStorage only if upstream still misses any residual race; much of it appears already upstreamed.


===================


part::formatVersion vs versionFormat etc. <- immutable now

CAFS



=============


7. **Independently** (no dependency on the above): delete the redundant
   `StorageReplicatedMergeTree::checkAlterPartitionIsPossible` override, and consider extracting
   the B37/B90/`LocalObjectStorage` robustness fixes as standalone upstream contributions.



====================



Почему это «не ломалось» на s3

Ломалось. Просто без исключений. Проверенная цепочка на сегодняшнем upstream-коде для plain s3:

1. Мутация всегда оборачивает storage нового парта в дисковую транзакцию (MutateTask.cpp:3153), и carried-forward hardlink'и проекций идут через executeWriteOperation → в транзакцию → в очередь (DataPartStorageOnDiskFull.cpp:307).
2. loadProjections в finalize (MutateTask.cpp:2264) проверяет existsDirectory("proj.proj") — видит только закоммиченную метадату → false.
3. Дальше — ключевое: if_not_loaded=true, check_consistency=false → проекция молча пропускается (IMergeTreeDataPart.cpp:1384-1418). Ни исключения, ни лога.

Итог на plain s3: мутированный парт живёт в памяти без carried-forward проекции (на диске после коммита файлы есть, в памяти парта — нет, до перезагрузки парта). И этот слепой in-memory образ дальше потребляют: MergedBlockOutputStream.cpp:222 (чексаммы финализируются по нему), MergeTask.cpp:1180 (следующий мерж решает «есть ли у парта проекция» по нему), DataPartsExchange.cpp:235 (fetch отправляет проекции по нему). То есть s3 «не ломался» только в смысле «никто не заметил»: деградация тихая — SELECT просто не использует проекцию для этого парта, следующий мерж может её тихо пересобрать-или-потерять. Комментарий в коммите B63 «on a plain disk the hardlinked dir is visible immediately» верен только для локального диска — там транзакция Fake и всё исполняется сразу.

Почему CA не может позволить себе ту же слепоту

У plain s3 два спасательных люка, и оба закрыты для CA одной причиной — whole-part атомарностью:

- Люк 1: early-commit под-транзакций. Read-back temp-проекций (мерж проекционных под-партов читает только что записанные блоки) на plain s3 работает потому, что под-транзакция проекции коммитится рано (MutateTask.cpp:1819) — её файлы становятся видимыми до коммита родителя. На CA ранний коммит под-парта = публикация полупарта = класс коррупции B21/B36. Запрещено. Значит под-парт едет в транзакции родителя, и его файлы до коммита читаемы только через оверлей транзакции.
- Люк 2: fail-open читатели, чья слепота стоит лишь перфа. На CA та же слепота стоит корректности: мы попробовали жить с ней через костыль (registerCarriedForwardProjectionForCA — регистрация проекции мимо диска) и получили B63 — тихо неверные агрегаты (rows_count=0 у carried-forward проекции, SELECT молча терял четверть суммы). А манифест и checksums.txt строятся из того, что видит транзакция, — слепота писателя даёт расходящийся опубликованный парт (B58: манифест без файлов проекции).


=================



 Suggested commit / diff split

For eventual upstreaming, these are independently reviewable and should be split out:
1. GCS conditional-write support (GCSConditionalDialect, GOOG4Signer, PocoHTTPClient HMAC) — self-contained, tested, consumable standalone.
2. renameParts disk-transaction-close durability fix — its own PR with non-CA regression tests.
3. ReadBufferFromFileView B115 position fix + gtest battery.
4. S3 412-no-retry policy + isPreconditionFailedError.
5. Core CAS backend (the rest).


==============


11. Non-CA-gated behavior changes to shared paths need their own sign-off when upstreaming
Risk score: 55 · Sources: compatibility
  - MergeTreeData::Transaction::renameParts now unconditionally commits every part's disk-storage transaction before the Keeper commit decision (the R3 acked-then-lost fix) — deliberate and correct-looking, but it reorders durability for all DiskObjectStorage-backed MergeTree writes; needs dedicated non-CA regression coverage (plain S3 + ReplicatedMergeTree, zero-copy) and should be presented as a standalone change upstream.
  - Client::RetryStrategy now never retries HTTP 412 globally (Client.cpp:104-111); the message-substring fallback in isPreconditionFailedError can over-match on S3-compatible backends. Also affects pre-existing Iceberg conditional writes (probably beneficially).
  - ReadBufferFromFileView position fix (B115) also changes the pre-existing packed skip-index reader; confirm that suite ran.


  =================



Продуктовый фикс (TDD, opus-ревью APPROVED): unwrapTableProxy во всех ~12 одно-табличных SYSTEM-командах с этим паттерном каста (SYNC/RESTORE/RESTART/DROP REPLICA, WAIT LOADING PARTS, PREWARM, ...), плюс stateless-тест 05017, который пинит регрессию (DETACH/ATTACH → SYNC REPLICA обязан пройти). Это апстрим-релевантный дефект — воспроизводится без CAS вообще. Ревью нашло ещё два pre-existing хвоста (whole-db DROP REPLICA молча скипает прокси; STOP <action> вешает ActionLock на прокси) — затрекано в BACKLOG.

3. Buffer-ctor дедлок rename-форварда (codex, верифицирован по коду) → откат; mutation-форвард остался (тест 05021).
4. Три продукт-бага lazy_load_tables + аудит (~60 непроброшенных виртуалов, backupData = молча пустой бэкап) → ждёт твоего решения (карантин vs ремедиация) — пункт в бэклоге, фича выключена в соаке.
===================


New document: docs/superpowers/cas/how-we-got-here.md — "CAS MergeTree — How We Got Here (by Trial and Error)", registered in the folder README's reading guide.

====


/home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak/scenarios/BACKLOG.md


============


RCA великолепный — и он оправдывает ветку: writeFileImpl — красная селёдка. Настоящая история: в тесте с 2024-07-29 сидит опечатка ("d/a" вместо "a/d/a"), которая всегда падала внутри, но clickhouse-disks --query глотал ошибку и выходил с кодом 0. Наш коммит ee80535672d (осознанный и правильный — exit-code контракт для ca-fsck) сделал давнюю поломку видимой. Продуктовый код не трогаем — чиним тест. Делаю фикс сразу.