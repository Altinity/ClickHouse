List of pull requests contributed by Altinity develoeprs to ClickHouse server. It does not include documentation changes.

## 2024
  * [Experiment] what if reorder compare of columns in Merge	[63780](https://github.com/ClickHouse/ClickHouse/pull/63780)	by @UnamedRus
  * Fix if condition in #63151	[73504](https://github.com/ClickHouse/ClickHouse/pull/73504)	by @zvonand
  * Make 01086_window_view_cleanup more stable	[72232](https://github.com/ClickHouse/ClickHouse/pull/72232)	by @ilejn
  * Fix #72756 (exception in RemoteQueryExecutor when user does not exist locally)	[72759](https://github.com/ClickHouse/ClickHouse/pull/72759)	by @zvonand
  * Respect `prefer_locahost_replica` in `parallel_distributed_insert_select`	[72190](https://github.com/ClickHouse/ClickHouse/pull/72190)	by @filimonov
  * Auxiliary autodiscovery	[71911](https://github.com/ClickHouse/ClickHouse/pull/71911)	by @ianton-ru
  * Passing external user roles from query originator to other nodes	[70332](https://github.com/ClickHouse/ClickHouse/pull/70332)	by @zvonand
  * Fix flaky test_drop_complex_columns	[71504](https://github.com/ClickHouse/ClickHouse/pull/71504)	by @ilejn
  * Boolean support for parquet native reader	[71055](https://github.com/ClickHouse/ClickHouse/pull/71055)	by @arthurpassos
  * Allow each authentication method to have its own expiration date, remove from user entity.	[70090](https://github.com/ClickHouse/ClickHouse/pull/70090)	by @arthurpassos
  * make numactl respect EPERM error, when get_mempolicy is is restricted by seccomp	[70900](https://github.com/ClickHouse/ClickHouse/pull/70900)	by @filimonov
  * add timeouts for retry loops in test_storage_rabbitmq	[70510](https://github.com/ClickHouse/ClickHouse/pull/70510)	by @filimonov
  * Support for Parquet page V2 on native reader	[70807](https://github.com/ClickHouse/ClickHouse/pull/70807)	by @arthurpassos
  * Add parquet bloom filters support	[62966](https://github.com/ClickHouse/ClickHouse/pull/62966)	by @arthurpassos
  * Thread pool: move thread creation out of lock	[68694](https://github.com/ClickHouse/ClickHouse/pull/68694)	by @filimonov
  * fix Kafka test	[70352](https://github.com/ClickHouse/ClickHouse/pull/70352)	by @filimonov
  * Maybe fix RabbitMQ	[70336](https://github.com/ClickHouse/ClickHouse/pull/70336)	by @filimonov
  * Add getSettingOrDefault() function	[69917](https://github.com/ClickHouse/ClickHouse/pull/69917)	by @shiyer7474
  * Fix REPLACE PARTITION waiting for mutations/merges on unrelated partitions 	[59138](https://github.com/ClickHouse/ClickHouse/pull/59138)	by @Enmk
  * Implement missing decimal cases for `zeroField` function by casting 0 to proper types	[69978](https://github.com/ClickHouse/ClickHouse/pull/69978)	by @arthurpassos
  * alpine docker images - use ubuntu 22.04 as glibc donor	[69033](https://github.com/ClickHouse/ClickHouse/pull/69033)	by @filimonov
  * Read only necessary columns & respect `ttl_only_drop_parts` on `materialize ttl`	[65488](https://github.com/ClickHouse/ClickHouse/pull/65488)	by @zvonand
  * Some fixes for LDAP	[68355](https://github.com/ClickHouse/ClickHouse/pull/68355)	by @zvonand
  * Multi auth methods	[65277](https://github.com/ClickHouse/ClickHouse/pull/65277)	by @arthurpassos
  * Do not use docker pause for Kerberos KDC container in integration tests	[69136](https://github.com/ClickHouse/ClickHouse/pull/69136)	by @ilejn
  * Fix test_role & test_keeper_s3_snapshot integration tests	[69013](https://github.com/ClickHouse/ClickHouse/pull/69013)	by @shiyer7474
  * Thread pool metrics	[68674](https://github.com/ClickHouse/ClickHouse/pull/68674)	by @filimonov
  * Fix subnet in docker_compose_net.yml	[69121](https://github.com/ClickHouse/ClickHouse/pull/69121)	by @ilejn
  * Fix small value DateTime64 constant folding in nested subquery for remote	[68323](https://github.com/ClickHouse/ClickHouse/pull/68323)	by @shiyer7474
  * Building aarch64 builds with '-no-pie' to allow better introspection	[67916](https://github.com/ClickHouse/ClickHouse/pull/67916)	by @filimonov
  * Add `**` to `hdfs` docs, add test for `**` in `hdfs`	[67064](https://github.com/ClickHouse/ClickHouse/pull/67064)	by @zvonand
  * Even better healthcheck for ldap	[67667](https://github.com/ClickHouse/ClickHouse/pull/67667)	by @zvonand
  * Fix serialization of parameterized view parameters	[67654](https://github.com/ClickHouse/ClickHouse/pull/67654)	by @shiyer7474
  * Skip parallel for `test_storage_kerberized_kafka`	[67349](https://github.com/ClickHouse/ClickHouse/pull/67349)	by @zvonand
  * [CI fest] Try to fix `test_broken_projections/test.py::test_broken_ignored_replicated`	[66915](https://github.com/ClickHouse/ClickHouse/pull/66915)	by @zvonand
  * Fix detection of number of CPUs in containers	[66237](https://github.com/ClickHouse/ClickHouse/pull/66237)	by @filimonov
  * Remove host regexp concurrent integration test	[66233](https://github.com/ClickHouse/ClickHouse/pull/66233)	by @arthurpassos
  * Minor: replaced expression with LEGACY_MAX_LEVEL	[61268](https://github.com/ClickHouse/ClickHouse/pull/61268)	by @Enmk
  * Build failure if ENABLE_AWS_S3 is OFF fixed	[66335](https://github.com/ClickHouse/ClickHouse/pull/66335)	by @ilejn
  * Try to make `test_ldap_external_user_directory` less flaky	[65794](https://github.com/ClickHouse/ClickHouse/pull/65794)	by @zvonand
  * Add `no_proxy` support	[63314](https://github.com/ClickHouse/ClickHouse/pull/63314)	by @arthurpassos
  * Add _time virtual column to file alike storages	[64947](https://github.com/ClickHouse/ClickHouse/pull/64947)	by @ilejn
  * Several minor fixes to proxy support in ClickHouse	[63427](https://github.com/ClickHouse/ClickHouse/pull/63427)	by @arthurpassos
  * Remove unused CaresPTRResolver::cancel_requests method	[63754](https://github.com/ClickHouse/ClickHouse/pull/63754)	by @arthurpassos
  * Improve S3 glob performance	[62120](https://github.com/ClickHouse/ClickHouse/pull/62120)	by @zvonand
  * Build kererberized_hadoop image by downloading commons-daemon via https	[62886](https://github.com/ClickHouse/ClickHouse/pull/62886)	by @ilejn
  * Do not allow table to be attached if there already is an active replica path	[61876](https://github.com/ClickHouse/ClickHouse/pull/61876)	by @arthurpassos
  * Add support for S3 access through aws private link interface [#62208](https://github.com/ClickHouse/ClickHouse/pull/62208) by @arthurpassos
  * Fix incorrect CI error message	[62139](https://github.com/ClickHouse/ClickHouse/pull/62139)	by @arthurpassos
  * Crash in Engine Merge if Row Policy does not have expression	[61971](https://github.com/ClickHouse/ClickHouse/pull/61971)	by @ilejn
  * Update storing-data.md	[62094](https://github.com/ClickHouse/ClickHouse/pull/62094)	by @filimonov
  * Reset part level upon attach from disk on MergeTree	[61536](https://github.com/ClickHouse/ClickHouse/pull/61536)	by @arthurpassos
  * update cppkafka to v0.4.1	[61119](https://github.com/ClickHouse/ClickHouse/pull/61119)	by @ilejn
  * Fix typo	[61540](https://github.com/ClickHouse/ClickHouse/pull/61540)	by @arthurpassos
  * Add mode fot topK/topKWeighed function to also include count/error statistics	[54508](https://github.com/ClickHouse/ClickHouse/pull/54508)	by @UnamedRus
  * Add test that validates attach partition fails if structure differs because of materialized column	[60418](https://github.com/ClickHouse/ClickHouse/pull/60418)	by @arthurpassos
  * Stateless test to validate projections work after attach	[60415](https://github.com/ClickHouse/ClickHouse/pull/60415)	by @arthurpassos
  * Allow to define `volume_priority` in `storage_configuration`	[58533](https://github.com/ClickHouse/ClickHouse/pull/58533)	by @zvonand
  * Revert revert attach partition PR	[59122](https://github.com/ClickHouse/ClickHouse/pull/59122)	by @arthurpassos
  * Update rename.md	[59017](https://github.com/ClickHouse/ClickHouse/pull/59017)	by @filimonov
  * Allow to attach partition from table with different partition expression when destination partition expression doesn't re-partition	[39507](https://github.com/ClickHouse/ClickHouse/pull/39507)	by @arthurpassos
  * Fixed potential exception due to stale profile UUID	[57263](https://github.com/ClickHouse/ClickHouse/pull/57263)	by @Enmk
  * Edit docs for toWeek()	[58452](https://github.com/ClickHouse/ClickHouse/pull/58452)	by @zvonand

## 2023
  * Fix `accurateCastOrNull` for out-of-range DateTime	[58139](https://github.com/ClickHouse/ClickHouse/pull/58139)	by @zvonand
  * Try to fix memory leak in StorageHDFS	[57860](https://github.com/ClickHouse/ClickHouse/pull/57860)	by @zvonand
  * Fix ThreadSanitizer data race in librdkafka 	[57791](https://github.com/ClickHouse/ClickHouse/pull/57791)	by @ilejn
  * Introduce `fileCluster` table function	[56868](https://github.com/ClickHouse/ClickHouse/pull/56868)	by @zvonand
  * Engine Merge obeys row policy	[50209](https://github.com/ClickHouse/ClickHouse/pull/50209)	by @ilejn
  * Sign all aws headers	[57001](https://github.com/ClickHouse/ClickHouse/pull/57001)	by @arthurpassos
  * Add transition from reading key to reading quoted key when double quotes are found	[56423](https://github.com/ClickHouse/ClickHouse/pull/56423)	by @arthurpassos
  * Performance enhancement for File, HDFS globs	[56141](https://github.com/ClickHouse/ClickHouse/pull/56141)	by @zvonand
  * Add ClickHouse setting to disable tunneling for HTTPS requests over HTTP proxy	[55033](https://github.com/ClickHouse/ClickHouse/pull/55033)	by @arthurpassos
  * add runOptimize call in bitmap write method( resubmit of #52842)	[55044](https://github.com/ClickHouse/ClickHouse/pull/55044)	by @UnamedRus
  * Introduce setting `date_time_overflow_behavior` to control the overflow behavior when converting to `Date` / `Date32` / `DateTime` / `DateTime64`	[55696](https://github.com/ClickHouse/ClickHouse/pull/55696)	by @zvonand
  * Improve parsing DateTime64 from timestamp represented as string	[55146](https://github.com/ClickHouse/ClickHouse/pull/55146)	by @zvonand
  * Disable logic max_threads=max_distributed_connections when async_socket_for_remote=1	[53504](https://github.com/ClickHouse/ClickHouse/pull/53504)	by @filimonov
  * Properly split remote proxy http https	[55430](https://github.com/ClickHouse/ClickHouse/pull/55430)	by @arthurpassos
  * Add option to set env variables for a single node in integration tests	[55208](https://github.com/ClickHouse/ClickHouse/pull/55208)	by @arthurpassos
  * Add sub-second precision to `formatReadableTimeDelta`	[54250](https://github.com/ClickHouse/ClickHouse/pull/54250)	by @zvonand
  * Refactor and simplify multi-directory globs	[54863](https://github.com/ClickHouse/ClickHouse/pull/54863)	by @zvonand
  * Fix directory permissions for multi-directory globs. Follow-up #50559	[52839](https://github.com/ClickHouse/ClickHouse/pull/52839)	by @zvonand
  * Account for monotonically increasing offsets across multiple batches on arrow-CH conversion	[54370](https://github.com/ClickHouse/ClickHouse/pull/54370)	by @arthurpassos
  * add runOptimize call in bitmap write method	[52842](https://github.com/ClickHouse/ClickHouse/pull/52842)	by @UnamedRus
  * Lower the number of iterations in test_host_regexp_multiple_ptr_records_concurrent	[54307](https://github.com/ClickHouse/ClickHouse/pull/54307)	by @arthurpassos
  * Minor clarifications to the `OPTIMIZE ... DEDUPLICATE` docs	[54257](https://github.com/ClickHouse/ClickHouse/pull/54257)	by @Enmk
  * Fix flaky test_s3_storage_conf_proxy	[54191](https://github.com/ClickHouse/ClickHouse/pull/54191)	by @arthurpassos
  * system.kafka_consumers exception info improvements	[53766](https://github.com/ClickHouse/ClickHouse/pull/53766)	by @ilejn
  * Try to fix bug with NULL::LowCardinality(Nullable(...)) NOT IN	[53706](https://github.com/ClickHouse/ClickHouse/pull/53706)	by @zvonand
  * Add global proxy setting	[51749](https://github.com/ClickHouse/ClickHouse/pull/51749)	by @arthurpassos
  * system.kafka_consumers table to monitor kafka consumers	[50999](https://github.com/ClickHouse/ClickHouse/pull/50999)	by @ilejn
  * Bring back **garbage** dns tests	[53286](https://github.com/ClickHouse/ClickHouse/pull/53286)	by @arthurpassos
  * Add option to switch float parsing methods	[52791](https://github.com/ClickHouse/ClickHouse/pull/52791)	by @zvonand
  * init and destroy ares channel on demand..	[52634](https://github.com/ClickHouse/ClickHouse/pull/52634)	by @arthurpassos
  * Fix `toDecimalString` function	[52520](https://github.com/ClickHouse/ClickHouse/pull/52520)	by @zvonand
  * Fixed inserting into Buffer engine	[52440](https://github.com/ClickHouse/ClickHouse/pull/52440)	by @Enmk
  * Fix typo last_removal_attemp_time	[52104](https://github.com/ClickHouse/ClickHouse/pull/52104)	by @filimonov
  * Basic auth to fetch Avro schema in Kafka	[49664](https://github.com/ClickHouse/ClickHouse/pull/49664)	by @ilejn
  * Add support for multi-directory globs	[50559](https://github.com/ClickHouse/ClickHouse/pull/50559)	by @zvonand
  * Small fix for toDateTime64() for dates after 2283-12-31	[52130](https://github.com/ClickHouse/ClickHouse/pull/52130)	by @zvonand
  * Small docs update for DateTime, DateTime64	[52094](https://github.com/ClickHouse/ClickHouse/pull/52094)	by @zvonand
  * Small docs update for toYearWeek() function	[52090](https://github.com/ClickHouse/ClickHouse/pull/52090)	by @zvonand
  * Accept key value delimiter as part of value	[49760](https://github.com/ClickHouse/ClickHouse/pull/49760)	by @arthurpassos
  * Row policy for database	[47640](https://github.com/ClickHouse/ClickHouse/pull/47640)	by @ilejn
  * Add setting to limit the max number of pairs produced by extractKeyVa…	[49836](https://github.com/ClickHouse/ClickHouse/pull/49836)	by @arthurpassos
  * sequence state fix	[48603](https://github.com/ClickHouse/ClickHouse/pull/48603)	by @ilejn
  * Secure in named collection	[46323](https://github.com/ClickHouse/ClickHouse/pull/46323)	by @ilejn
  * Refactor reading the pool settings from server config	[48055](https://github.com/ClickHouse/ClickHouse/pull/48055)	by @filimonov
  * Add toDecimalString function	[47838](https://github.com/ClickHouse/ClickHouse/pull/47838)	by @zvonand
  * Add unit test to assert arrow lib does not abort on fatal logs	[47958](https://github.com/ClickHouse/ClickHouse/pull/47958)	by @arthurpassos
  * Tests for orphaned role fix	[47002](https://github.com/ClickHouse/ClickHouse/pull/47002)	by @ilejn
  * Simplify clickhouse-test usage with -b	[47578](https://github.com/ClickHouse/ClickHouse/pull/47578)	by @Enmk
  * Modify find_first_symbols so it works as expected for find_first_not_symbols	[47304](https://github.com/ClickHouse/ClickHouse/pull/47304)	by @arthurpassos
  * Add user setting to force select final at query level	[40945](https://github.com/ClickHouse/ClickHouse/pull/40945)	by @arthurpassos
  * Attempt to fix 'Local: No offset stored message' from Kafka	[42391](https://github.com/ClickHouse/ClickHouse/pull/42391)	by @filimonov
  * Cancel c-ares failed requests and retry on system interrupts to prevent callbacks with dangling references and premature resolution failures	[45629](https://github.com/ClickHouse/ClickHouse/pull/45629)	by @arthurpassos

## 2022
  * Fixed exception when user tries to log in	[42641](https://github.com/ClickHouse/ClickHouse/pull/42641)	by @Enmk
  * Flatten list type arrow chunks on parsing	[43297](https://github.com/ClickHouse/ClickHouse/pull/43297)	by @arthurpassos
  * Added precise decimal multiplication and division	[42438](https://github.com/ClickHouse/ClickHouse/pull/42438)	by @zvonand
  * fix LDAP in case of many roles on user	[42461](https://github.com/ClickHouse/ClickHouse/pull/42461)	by @Enmk
  * Add SensitiveDataMasker to exceptions messages	[42940](https://github.com/ClickHouse/ClickHouse/pull/42940)	by @filimonov
  * Allow autoremoval of old parts if detach_not_byte_identical_parts enabled	[43287](https://github.com/ClickHouse/ClickHouse/pull/43287)	by @filimonov
  * Added applied row-level policies to `system.query_log`	[39819](https://github.com/ClickHouse/ClickHouse/pull/39819)	by @quickhouse
  * Fix Polygon dict xml config	[42773](https://github.com/ClickHouse/ClickHouse/pull/42773)	by @UnamedRus
  * Fix c-ares crash	[42234](https://github.com/ClickHouse/ClickHouse/pull/42234)	by @arthurpassos
  * Fix incorrect trace log line on dict reload	[42609](https://github.com/ClickHouse/ClickHouse/pull/42609)	by @filimonov
  * Fix Date from CSV parsing	[42044](https://github.com/ClickHouse/ClickHouse/pull/42044)	by @zvonand
  * Increase request_timeout_ms for s3 disks.	[42321](https://github.com/ClickHouse/ClickHouse/pull/42321)	by @filimonov
  * Test for ignore function in PARTITION KEY	[39875](https://github.com/ClickHouse/ClickHouse/pull/39875)	by @UnamedRus
  * Remove obsolete comment from the config.xml	[41518](https://github.com/ClickHouse/ClickHouse/pull/41518)	by @filimonov
  * Add Parquet Time32/64 conversion to CH DateTime32/64	[41333](https://github.com/ClickHouse/ClickHouse/pull/41333)	by @arthurpassos
  * CaresPTRResolver small safety improvement	[40890](https://github.com/ClickHouse/ClickHouse/pull/40890)	by @arthurpassos
  * Add support for extended (chunked) arrays for Parquet format	[40485](https://github.com/ClickHouse/ClickHouse/pull/40485)	by @arthurpassos
  * Fixed `Unknown identifier (aggregate-function)` exception which appears when a user tries to calculate WINDOW ORDER BY/PARTITION BY expressions over aggregate functions	[39762](https://github.com/ClickHouse/ClickHouse/pull/39762)	by @quickhouse
  * Fix docs for Base58	[40798](https://github.com/ClickHouse/ClickHouse/pull/40798)	by @zvonand
  * Fix CaresPTRResolver not reading hosts file	[40769](https://github.com/ClickHouse/ClickHouse/pull/40769)	by @arthurpassos
  * Fix conversion Date32 / DateTime64 / Date to narrow types	[40217](https://github.com/ClickHouse/ClickHouse/pull/40217)	by @zvonand
  * Base58 fix handling leading 0 / '1'	[40620](https://github.com/ClickHouse/ClickHouse/pull/40620)	by @zvonand
  * Fixed point of origin for exponential decay window functions to the last value in window	[39593](https://github.com/ClickHouse/ClickHouse/pull/39593)	by @quickhouse
  * Check Decimal division overflow based on operands scale	[39600](https://github.com/ClickHouse/ClickHouse/pull/39600)	by @zvonand
  * Fix arrow column dictionary to ch lc	[40037](https://github.com/ClickHouse/ClickHouse/pull/40037)	by @arthurpassos
  * Unwrap LC column in IExecutablefunction::executeWithoutSparseColumns	[39716](https://github.com/ClickHouse/ClickHouse/pull/39716)	by @arthurpassos
  * Fixed regexp in `test_match_process_uid_against_data_owner`	[39085](https://github.com/ClickHouse/ClickHouse/pull/39085)	by @quickhouse
  * Fix timeSlots for DateTime64	[37951](https://github.com/ClickHouse/ClickHouse/pull/37951)	by @zvonand
  * Fixed regexp in `test_quota`	[39084](https://github.com/ClickHouse/ClickHouse/pull/39084)	by @quickhouse
  * Fix bug with maxsplit in the splitByChar	[39552](https://github.com/ClickHouse/ClickHouse/pull/39552)	by @filimonov
  * Uppercase `ROWS`, `GROUPS`, `RANGE` in queries with windows	[39410](https://github.com/ClickHouse/ClickHouse/pull/39410)	by @quickhouse
  * Simplify Base58 encoding/decoding	[39292](https://github.com/ClickHouse/ClickHouse/pull/39292)	by @zvonand
  * Test host_regexp against all PTR records instead of only one	[37827](https://github.com/ClickHouse/ClickHouse/pull/37827)	by @arthurpassos
  * Add check for empty proccessors in AggregatingTransform::expandPipeline	[38584](https://github.com/ClickHouse/ClickHouse/pull/38584)	by @filimonov
  * Fix exception messages in clickhouse su	[38619](https://github.com/ClickHouse/ClickHouse/pull/38619)	by @filimonov
  * Don't spoil return code of integration tests runner with redundant tee	[38548](https://github.com/ClickHouse/ClickHouse/pull/38548)	by @excitoon
  * Add Base58 encoding/decoding	[38159](https://github.com/ClickHouse/ClickHouse/pull/38159)	by @zvonand
  * Fixed comments	[38331](https://github.com/ClickHouse/ClickHouse/pull/38331)	by @excitoon
  * nonNegativeDerivative docs description	[38303](https://github.com/ClickHouse/ClickHouse/pull/38303)	by @zvonand
  * Optimized processing of ORDER BY in window functions	[34632](https://github.com/ClickHouse/ClickHouse/pull/34632)	by @excitoon
  * fix deadlink with the proper one	[37883](https://github.com/ClickHouse/ClickHouse/pull/37883)	by @filimonov
  * Non Negative Derivative window function	[37628](https://github.com/ClickHouse/ClickHouse/pull/37628)	by @zvonand
  * Better parsing of `versionId` in `S3::URI::URI`	[37964](https://github.com/ClickHouse/ClickHouse/pull/37964)	by @excitoon
  * Don't try to kill empty list of containers in `integration/runner`	[37854](https://github.com/ClickHouse/ClickHouse/pull/37854)	by @excitoon
  * Got rid of `S3AuthSigner`	[37769](https://github.com/ClickHouse/ClickHouse/pull/37769)	by @excitoon
  * Fix for exponential time decaying window functions	[36944](https://github.com/ClickHouse/ClickHouse/pull/36944)	by @excitoon
  * Moved `ClientConfigurationPerRequest` to ClickHouse	[37767](https://github.com/ClickHouse/ClickHouse/pull/37767)	by @excitoon
  * Typos	[37773](https://github.com/ClickHouse/ClickHouse/pull/37773)	by @excitoon
  * Implemented changing comment to a ReplicatedMergeTree table	[37416](https://github.com/ClickHouse/ClickHouse/pull/37416)	by @Enmk
  * Fixed error with symbols in key name in S3	[37344](https://github.com/ClickHouse/ClickHouse/pull/37344)	by @excitoon
  * Fixed problem with infs in `quantileTDigest`	[37021](https://github.com/ClickHouse/ClickHouse/pull/37021)	by @excitoon
  * Mention currentDatabase in the Buffer docs	[36962](https://github.com/ClickHouse/ClickHouse/pull/36962)	by @filimonov
  * Fixed missing enum values for ClientInfo::Interface	[36482](https://github.com/ClickHouse/ClickHouse/pull/36482)	by @Enmk
  * Set `ENABLE_BUILD_PATH_MAPPING` to `OFF` by default, if `CMAKE_BUILD_TYPE` is set to `Debug`	[35998](https://github.com/ClickHouse/ClickHouse/pull/35998)	by @traceon
  * Add some metrics to engine Kafka	[35916](https://github.com/ClickHouse/ClickHouse/pull/35916)	by @filimonov
  * Fix cgroups cores detection	[35815](https://github.com/ClickHouse/ClickHouse/pull/35815)	by @filimonov
  * Extended usage of Milliseconds, Microseconds, Nanoseconds 	[34353](https://github.com/ClickHouse/ClickHouse/pull/34353)	by @zvonand
  * asynchronous_inserts engine AsynchronousInserts -> SystemAsynchronousInserts	[34429](https://github.com/ClickHouse/ClickHouse/pull/34429)	by @filimonov
  * Fix fromUnixTimestamp64 functions	[33505](https://github.com/ClickHouse/ClickHouse/pull/33505)	by @zvonand
  * Fix LDAP and Kerberos config handling	[33689](https://github.com/ClickHouse/ClickHouse/pull/33689)	by @traceon

## 2021
  * Kerberos docs - formatting fixes	[33011](https://github.com/ClickHouse/ClickHouse/pull/33011)	by @traceon
  * Views with comment	[31062](https://github.com/ClickHouse/ClickHouse/pull/31062)	by @Enmk
  * Test for issue #26643	[27822](https://github.com/ClickHouse/ClickHouse/pull/27822)	by @filimonov
  * Minor fixes for `StorageMergeTree`	[32037](https://github.com/ClickHouse/ClickHouse/pull/32037)	by @excitoon
  * Skip mutations of unrelated partitions in `StorageMergeTree`	[21326](https://github.com/ClickHouse/ClickHouse/pull/21326)	by @excitoon
  * Give some love to macOS platform	[31957](https://github.com/ClickHouse/ClickHouse/pull/31957)	by @traceon
  * Minor improvements to DUMP macro	[31858](https://github.com/ClickHouse/ClickHouse/pull/31858)	by @Enmk
  * Windowed time decay functions	[29799](https://github.com/ClickHouse/ClickHouse/pull/29799)	by @excitoon
  * Fixed null pointer exception in `MATERIALIZE COLUMN`	[31679](https://github.com/ClickHouse/ClickHouse/pull/31679)	by @excitoon
  * Update string-search-functions.md	[31632](https://github.com/ClickHouse/ClickHouse/pull/31632)	by @filimonov
  * Resolve `nullptr` in STS credentials provider for S3	[31409](https://github.com/ClickHouse/ClickHouse/pull/31409)	by @excitoon
  * (manual backport) Avoid crashes from parallel_view_processing 	[30472](https://github.com/ClickHouse/ClickHouse/pull/30472)	by @filimonov
  * Fixing query performance issue in Live Views	[31006](https://github.com/ClickHouse/ClickHouse/pull/31006)	by @vzakaznikov
  * Better handling of `xtables.lock` in `runner`	[30892](https://github.com/ClickHouse/ClickHouse/pull/30892)	by @excitoon
  * Fixed `--disable-net-host` in `runner`	[30863](https://github.com/ClickHouse/ClickHouse/pull/30863)	by @excitoon
  * Non-recursive implementation for type list and its functions	[29683](https://github.com/ClickHouse/ClickHouse/pull/29683)	by @myrrc
  * Implemented creating databases with comments	[29429](https://github.com/ClickHouse/ClickHouse/pull/29429)	by @Enmk
  * Fix Xcode 13 build	[29682](https://github.com/ClickHouse/ClickHouse/pull/29682)	by @traceon
  * More warning flags for clang	[29668](https://github.com/ClickHouse/ClickHouse/pull/29668)	by @myrrc
  * Introducing Fn concept for function signature checking, simplifying SimpleCache	[29500](https://github.com/ClickHouse/ClickHouse/pull/29500)	by @myrrc
  * Making Monotonicity an aggregate to use with designated initializers	[29540](https://github.com/ClickHouse/ClickHouse/pull/29540)	by @myrrc
  * Implemented modifying table comments with `ALTER TABLE t MODIFY COMMENT 'value'`	[29264](https://github.com/ClickHouse/ClickHouse/pull/29264)	by @Enmk
  * Better exception messages for some String-related functions	[29252](https://github.com/ClickHouse/ClickHouse/pull/29252)	by @Enmk
  * Fixed logging level for message in `S3Common.cpp`	[29308](https://github.com/ClickHouse/ClickHouse/pull/29308)	by @excitoon
  * Removed sub-optimal mutation notifications in `StorageMergeTree` when merges are still going	[27552](https://github.com/ClickHouse/ClickHouse/pull/27552)	by @excitoon
  * Improving CH type system with concepts	[28659](https://github.com/ClickHouse/ClickHouse/pull/28659)	by @myrrc
  * optional<> semantics for parsing MergeTreePartInfo and DetachedPartInfo	[28085](https://github.com/ClickHouse/ClickHouse/pull/28085)	by @myrrc
  * Governance/session log	[22415](https://github.com/ClickHouse/ClickHouse/pull/22415)	by @Enmk
  * `ALTER TABLE ... MATERIALIZE COLUMN`	[27038](https://github.com/ClickHouse/ClickHouse/pull/27038)	by @excitoon
  * Fixed a typo in comments to `SinkToStorage`	[28078](https://github.com/ClickHouse/ClickHouse/pull/28078)	by @excitoon
  * Don't silently ignore errors and don't count delays in `ReadBufferFromS3`	[27484](https://github.com/ClickHouse/ClickHouse/pull/27484)	by @excitoon
  * Partitioned write into s3 table function	[23051](https://github.com/ClickHouse/ClickHouse/pull/23051)	by @excitoon
  * S3 disk unstable reads test	[27176](https://github.com/ClickHouse/ClickHouse/pull/27176)	by @excitoon
  * Avoid nullptr dereference during processing of NULL messages in Kafka for some formats	[27794](https://github.com/ClickHouse/ClickHouse/pull/27794)	by @filimonov
  * less sys calls #2: make vdso work again	[27492](https://github.com/ClickHouse/ClickHouse/pull/27492)	by @filimonov
  * Fixed parsing DateTime64 value from string.	[27605](https://github.com/ClickHouse/ClickHouse/pull/27605)	by @Enmk
  * Less Stopwatch.h	[27569](https://github.com/ClickHouse/ClickHouse/pull/27569)	by @filimonov
  * Changelog for 21.8	[27271](https://github.com/ClickHouse/ClickHouse/pull/27271)	by @filimonov
  * Improved logging of `hwmon` sensor errors in `AsynchronousMetrics`	[27031](https://github.com/ClickHouse/ClickHouse/pull/27031)	by @excitoon
  * Using formatted string literals in clickhouse-test, extracted sort key functions and stacktraces printer	[27228](https://github.com/ClickHouse/ClickHouse/pull/27228)	by @myrrc
  * Fixing reading of /proc/meminfo when kB suffix is not present	[27361](https://github.com/ClickHouse/ClickHouse/pull/27361)	by @myrrc
  * Less clock_gettime calls	[27325](https://github.com/ClickHouse/ClickHouse/pull/27325)	by @filimonov
  * Update changelog/README.md	[27221](https://github.com/ClickHouse/ClickHouse/pull/27221)	by @filimonov
  * Safer `ReadBufferFromS3` for merges and backports	[27168](https://github.com/ClickHouse/ClickHouse/pull/27168)	by @excitoon
  * Moving to TestFlows 1.7.20 that has native support for parallel tests.	[27040](https://github.com/ClickHouse/ClickHouse/pull/27040)	by @vzakaznikov
  * Updated extractAllGroupsHorizontal - flexible limit on number of matches	[26961](https://github.com/ClickHouse/ClickHouse/pull/26961)	by @Enmk
  * Improved `runner` to use `pytest` keyword expressions	[27026](https://github.com/ClickHouse/ClickHouse/pull/27026)	by @excitoon
  * Enabling RBAC TestFlows tests and crossing out new fails.	[26747](https://github.com/ClickHouse/ClickHouse/pull/26747)	by @vzakaznikov
  * Update error message in tests/testflows/window_functions/tests/errors.py	[26744](https://github.com/ClickHouse/ClickHouse/pull/26744)	by @vzakaznikov
  * Fixed wrong error message in `S3Common`	[26738](https://github.com/ClickHouse/ClickHouse/pull/26738)	by @excitoon
  * Enabling all TestFlows modules except LDAP after Kerberos merge.	[26366](https://github.com/ClickHouse/ClickHouse/pull/26366)	by @vzakaznikov
  * Enabling Kerberos Testflows tests	[21659](https://github.com/ClickHouse/ClickHouse/pull/21659)	by @zvonand
  * Fixing RBAC sample by tests in TestFlows.	[26329](https://github.com/ClickHouse/ClickHouse/pull/26329)	by @vzakaznikov
  * Using required columns for block size prediction	[25917](https://github.com/ClickHouse/ClickHouse/pull/25917)	by @excitoon
  * WIP mark subset of tests that depend on specific config modifications	[26105](https://github.com/ClickHouse/ClickHouse/pull/26105)	by @myrrc
  * Disabling TestFlows LDAP module due to test fails.	[26065](https://github.com/ClickHouse/ClickHouse/pull/26065)	by @vzakaznikov
  * Enabling all TestFlows modules and fixing some tests.	[26011](https://github.com/ClickHouse/ClickHouse/pull/26011)	by @vzakaznikov
  * Guidelines for adding new third-party libraries and maintaining custom changes in them	[26009](https://github.com/ClickHouse/ClickHouse/pull/26009)	by @traceon
  * Fix native macOS (Xcode) builds	[25736](https://github.com/ClickHouse/ClickHouse/pull/25736)	by @traceon
  * Changed css theme for code highlighting	[25682](https://github.com/ClickHouse/ClickHouse/pull/25682)	by @myrrc
  * Enabling TestFlows RBAC tests.	[25498](https://github.com/ClickHouse/ClickHouse/pull/25498)	by @vzakaznikov
  * SYSTEM RESTORE REPLICA replica [ON CLUSTER cluster]	[13652](https://github.com/ClickHouse/ClickHouse/pull/13652)	by @myrrc
  * TestFlows: increase LDAP verification cooldown performance tests timeout to 600 sec	[25374](https://github.com/ClickHouse/ClickHouse/pull/25374)	by @vzakaznikov
  * Enable TestFlows LDAP tests	[25278](https://github.com/ClickHouse/ClickHouse/pull/25278)	by @vzakaznikov
  * Kafka failover issue fix	[21267](https://github.com/ClickHouse/ClickHouse/pull/21267)	by @filimonov
  * Improved `test_storage_s3_get_unstable`	[23976](https://github.com/ClickHouse/ClickHouse/pull/23976)	by @excitoon
  * Adding leadInFrame/lagInFrame window functions TestFlows tests	[25144](https://github.com/ClickHouse/ClickHouse/pull/25144)	by @vzakaznikov
  * Fixed problems with double spaces in logs for `S3Common`	[24897](https://github.com/ClickHouse/ClickHouse/pull/24897)	by @excitoon
  * Added support of hasAny function to bloom_filter index.	[24900](https://github.com/ClickHouse/ClickHouse/pull/24900)	by @Enmk
  * Fixed bug with declaring S3 disk at root of bucket	[24898](https://github.com/ClickHouse/ClickHouse/pull/24898)	by @excitoon
  * Disabling extended precision data types TestFlows tests	[25125](https://github.com/ClickHouse/ClickHouse/pull/25125)	by @vzakaznikov
  * Fix using Yandex dockerhub registries for TestFlows.	[25133](https://github.com/ClickHouse/ClickHouse/pull/25133)	by @vzakaznikov
  * Add test issue #23430	[24941](https://github.com/ClickHouse/ClickHouse/pull/24941)	by @filimonov
  * Update CHANGELOG.md	[25083](https://github.com/ClickHouse/ClickHouse/pull/25083)	by @filimonov
  * Fix the test after #20393	[24967](https://github.com/ClickHouse/ClickHouse/pull/24967)	by @filimonov
  * Fix bad error message in docker entrypoint	[24955](https://github.com/ClickHouse/ClickHouse/pull/24955)	by @filimonov
  * Ldap role mapping deadlock fix	[24431](https://github.com/ClickHouse/ClickHouse/pull/24431)	by @traceon
  * Adding support to save clickhouse server logs in TestFlows check	[24504](https://github.com/ClickHouse/ClickHouse/pull/24504)	by @vzakaznikov
  * Try to improve kafka flaky test	[24465](https://github.com/ClickHouse/ClickHouse/pull/24465)	by @filimonov
  * LDAP: user DN detection functionality for role mapping with Active Directory	[22228](https://github.com/ClickHouse/ClickHouse/pull/22228)	by @traceon
  * Different loglevels for different logging channels	[23857](https://github.com/ClickHouse/ClickHouse/pull/23857)	by @filimonov
  * Fixed now64(): added second optional argument for timezone.	[24091](https://github.com/ClickHouse/ClickHouse/pull/24091)	by @Enmk
  * Enabling running of all TestFlows modules in parallel	[23942](https://github.com/ClickHouse/ClickHouse/pull/23942)	by @vzakaznikov
  * Better handling of HTTP errors in `PocoHTTPClient`	[23844](https://github.com/ClickHouse/ClickHouse/pull/23844)	by @excitoon
  * Added `region` parameter for S3 storage and disk	[23846](https://github.com/ClickHouse/ClickHouse/pull/23846)	by @excitoon
  * Documentation fix for `quantileTDigestWeighted`	[23758](https://github.com/ClickHouse/ClickHouse/pull/23758)	by @excitoon
  * Fixing testflows window function distributed tests	[23975](https://github.com/ClickHouse/ClickHouse/pull/23975)	by @vzakaznikov
  * Testflows tests for DateTime64 extended range	[22729](https://github.com/ClickHouse/ClickHouse/pull/22729)	by @zvonand
  * Added solution for host network mode in Ubuntu 20.10+	[23939](https://github.com/ClickHouse/ClickHouse/pull/23939)	by @excitoon
  * TestFlows window functions tests	[23704](https://github.com/ClickHouse/ClickHouse/pull/23704)	by @vzakaznikov
  * Update librdkafka 1.6.0-RC3 to 1.6.1	[23874](https://github.com/ClickHouse/ClickHouse/pull/23874)	by @filimonov
  * autodetect arch of gosu in server dockerfile	[23802](https://github.com/ClickHouse/ClickHouse/pull/23802)	by @filimonov
  * [ci run] Tdigest fix to 21.1	[23360](https://github.com/ClickHouse/ClickHouse/pull/23360)	by @excitoon
  * Fixed `quantile(s)TDigest` inaccuracies	[23314](https://github.com/ClickHouse/ClickHouse/pull/23314)	by @excitoon
  * Fix restart / stop command hanging.	[23552](https://github.com/ClickHouse/ClickHouse/pull/23552)	by @filimonov
  * Adding Map type tests in TestFlows	[21087](https://github.com/ClickHouse/ClickHouse/pull/21087)	by @vzakaznikov
  * Disable clickhouse-odbc-bridge build when ODBC is disabled	[23357](https://github.com/ClickHouse/ClickHouse/pull/23357)	by @traceon
  * Fix AppleClang build	[23358](https://github.com/ClickHouse/ClickHouse/pull/23358)	by @traceon
  * Retries on HTTP connection drops during reads from S3	[22988](https://github.com/ClickHouse/ClickHouse/pull/22988)	by @excitoon
  * Added insecure IMDS credentials provider for S3	[21852](https://github.com/ClickHouse/ClickHouse/pull/21852)	by @excitoon
  * Consistent AWS timeouts	[22594](https://github.com/ClickHouse/ClickHouse/pull/22594)	by @excitoon
  * Fixed erroneus failure of extractAllGroupsHorizontal on large columns	[23036](https://github.com/ClickHouse/ClickHouse/pull/23036)	by @Enmk
  * Fixes: formatDateTime and toDateTime64	[22937](https://github.com/ClickHouse/ClickHouse/pull/22937)	by @Enmk
  * Fixed dateDiff for DateTime64	[22931](https://github.com/ClickHouse/ClickHouse/pull/22931)	by @Enmk
  * Updated toStartOf* docs	[22935](https://github.com/ClickHouse/ClickHouse/pull/22935)	by @zvonand
  * Fix vanilla GCC compilation in macOS	[22885](https://github.com/ClickHouse/ClickHouse/pull/22885)	by @traceon
  * Fix issue with quorum retries behaviour	[18215](https://github.com/ClickHouse/ClickHouse/pull/18215)	by @filimonov
  * Better tests for finalize in nested writers	[22110](https://github.com/ClickHouse/ClickHouse/pull/22110)	by @excitoon
  * AppleClang compilation fix	[22561](https://github.com/ClickHouse/ClickHouse/pull/22561)	by @traceon
  * Revisit macOS build instructions	[22508](https://github.com/ClickHouse/ClickHouse/pull/22508)	by @traceon
  * Lookup parts/partitions in replica's own detached/ folder before downloading them from other replicas	[18978](https://github.com/ClickHouse/ClickHouse/pull/18978)	by @myrrc
  * Fix native macOS build for ALL_BUILD (Xcode/AppleClang)	[22289](https://github.com/ClickHouse/ClickHouse/pull/22289)	by @traceon
  * Add suffixes for dockerfile arguments	[22301](https://github.com/ClickHouse/ClickHouse/pull/22301)	by @filimonov
  * Add possibility to customize the source for clickhouse-server docker image builds.	[21977](https://github.com/ClickHouse/ClickHouse/pull/21977)	by @filimonov
  * Update column.md	[22061](https://github.com/ClickHouse/ClickHouse/pull/22061)	by @filimonov
  * docker: avoid chown of .	[22102](https://github.com/ClickHouse/ClickHouse/pull/22102)	by @filimonov
  * Adding documentation for Kerberos external authenticator.	[21328](https://github.com/ClickHouse/ClickHouse/pull/21328)	by @zvonand
  * Updating docker/test/testflows/runner/dockerd-entrypoint.sh to use Yandex dockerhub-proxy	[21551](https://github.com/ClickHouse/ClickHouse/pull/21551)	by @vzakaznikov
  * Documentation on OPTIMIZE DEDUPLICATE BY expression.	[21739](https://github.com/ClickHouse/ClickHouse/pull/21739)	by @Enmk
  * Date time64 extended range	[9404](https://github.com/ClickHouse/ClickHouse/pull/9404)	by @Enmk
  * Reverted S3 connection pools	[21737](https://github.com/ClickHouse/ClickHouse/pull/21737)	by @excitoon
  * Updating TestFlows to 1.6.74	[21673](https://github.com/ClickHouse/ClickHouse/pull/21673)	by @vzakaznikov
  * Added Grant, Revoke and System query_kind for system.query_log	[21102](https://github.com/ClickHouse/ClickHouse/pull/21102)	by @Enmk
  * Fixed open behavior of remote host filter in case when there is `remote_url_allow_hosts` section in configuration but no entries there	[20058](https://github.com/ClickHouse/ClickHouse/pull/20058)	by @excitoon
  * Add Kerberos support for authenticating existing users when accessing over HTTP	[14995](https://github.com/ClickHouse/ClickHouse/pull/14995)	by @traceon
  * Fixing LDAP authentication performance test by removing assertion	[21507](https://github.com/ClickHouse/ClickHouse/pull/21507)	by @vzakaznikov
  * Adjust prewhere_with_row_level_filter performance test	[21442](https://github.com/ClickHouse/ClickHouse/pull/21442)	by @traceon
  * avoid race in librdkafka	[21452](https://github.com/ClickHouse/ClickHouse/pull/21452)	by @filimonov
  * Case-insensitive compression methods for table functions	[21416](https://github.com/ClickHouse/ClickHouse/pull/21416)	by @excitoon
  * Allow row policies with PREWHERE	[19576](https://github.com/ClickHouse/ClickHouse/pull/19576)	by @traceon
  * Better kafka tests	[21111](https://github.com/ClickHouse/ClickHouse/pull/21111)	by @filimonov
  * Adding documentation for LIVE VIEWs	[20217](https://github.com/ClickHouse/ClickHouse/pull/20217)	by @vzakaznikov
  * Adding documentation on how to use LDAP server as external user authenticator or directory	[20208](https://github.com/ClickHouse/ClickHouse/pull/20208)	by @vzakaznikov
  * Added Server Side Encryption Customer Keys support in S3 client	[19748](https://github.com/ClickHouse/ClickHouse/pull/19748)	by @excitoon
  * Add libnss_files to alpine image	[20336](https://github.com/ClickHouse/ClickHouse/pull/20336)	by @filimonov
  * Add example of client configuration adjustments	[20275](https://github.com/ClickHouse/ClickHouse/pull/20275)	by @filimonov
  * Update entrypoint.sh	[20012](https://github.com/ClickHouse/ClickHouse/pull/20012)	by @filimonov
  * Adding support for periodically refreshed LIVE VIEW tables	[14822](https://github.com/ClickHouse/ClickHouse/pull/14822)	by @vzakaznikov
  * Adding retries for docker-compose start, stop and restart in TestFlows tests	[19852](https://github.com/ClickHouse/ClickHouse/pull/19852)	by @vzakaznikov
  * Try to make test_dir.tar smaller	[19833](https://github.com/ClickHouse/ClickHouse/pull/19833)	by @filimonov
  * Used global region for accessing S3 if can't determine exactly	[19750](https://github.com/ClickHouse/ClickHouse/pull/19750)	by @excitoon
  * Fixed table function S3 `auto` compression mode	[19793](https://github.com/ClickHouse/ClickHouse/pull/19793)	by @excitoon
  * Updated docs on encrypt/decrypt functions	[19819](https://github.com/ClickHouse/ClickHouse/pull/19819)	by @Enmk
  * Update of AWS C++ SDK	[17870](https://github.com/ClickHouse/ClickHouse/pull/17870)	by @excitoon
  * Updating TestFlows AES encryption tests to support changes to the encrypt plaintext parameter.	[19674](https://github.com/ClickHouse/ClickHouse/pull/19674)	by @vzakaznikov
  * Added prefix-based S3 endpoint settings	[18812](https://github.com/ClickHouse/ClickHouse/pull/18812)	by @excitoon
  * LDAP group to local role mapping support	[17211](https://github.com/ClickHouse/ClickHouse/pull/17211)	by @traceon
  * Kafka for arm64	[19369](https://github.com/ClickHouse/ClickHouse/pull/19369)	by @filimonov
  * Allow docker to be executed with arbitrary uid	[19374](https://github.com/ClickHouse/ClickHouse/pull/19374)	by @filimonov
  * Allow building librdkafka without ssl	[19337](https://github.com/ClickHouse/ClickHouse/pull/19337)	by @filimonov
  * Update librdkafka to v1.6.0-RC2	[18671](https://github.com/ClickHouse/ClickHouse/pull/18671)	by @filimonov
  * Connection pools for S3	[13405](https://github.com/ClickHouse/ClickHouse/pull/13405)	by @excitoon
  * Docker: fix uid/gid of the clickhouse user	[19096](https://github.com/ClickHouse/ClickHouse/pull/19096)	by @filimonov
  * Update test containers	[19058](https://github.com/ClickHouse/ClickHouse/pull/19058)	by @filimonov
  * Docker: better clickhouse-server  entrypoint	[18954](https://github.com/ClickHouse/ClickHouse/pull/18954)	by @filimonov
  * arrayMin/Max/Sum - fix bad description, add examples	[18833](https://github.com/ClickHouse/ClickHouse/pull/18833)	by @filimonov
  * Fixed GCC coverage build	[18846](https://github.com/ClickHouse/ClickHouse/pull/18846)	by @myrrc
  * TestFlows: fixes to LDAP tests that fail due to slow test execution	[18790](https://github.com/ClickHouse/ClickHouse/pull/18790)	by @vzakaznikov
  * Check if XCODE_IDE is true and avoid enforcing ninja in that case	[18773](https://github.com/ClickHouse/ClickHouse/pull/18773)	by @traceon
  * Fix AppleClang compilation - Remove auto in function parameters	[18674](https://github.com/ClickHouse/ClickHouse/pull/18674)	by @traceon

## 2020
  * Update build instructions for clang-11	[18642](https://github.com/ClickHouse/ClickHouse/pull/18642)	by @filimonov
  * Allow caching of successful "bind" attempts to LDAP server for configurable period of time	[15988](https://github.com/ClickHouse/ClickHouse/pull/15988)	by @traceon
  * Allow multiplication of Decimal and Float	[18145](https://github.com/ClickHouse/ClickHouse/pull/18145)	by @myrrc
  * Docs for table, column, database names passed as parameters.	[18519](https://github.com/ClickHouse/ClickHouse/pull/18519)	by @UnamedRus
  * Allow AppleClang builds	[18488](https://github.com/ClickHouse/ClickHouse/pull/18488)	by @traceon
  * Fix exception text from Pipe.cpp	[18396](https://github.com/ClickHouse/ClickHouse/pull/18396)	by @filimonov
  * Perf test for ColumnMap	[18317](https://github.com/ClickHouse/ClickHouse/pull/18317)	by @Enmk
  * Fixed Date vs DateTime64 comparison	[18050](https://github.com/ClickHouse/ClickHouse/pull/18050)	by @Enmk
  * Fixed flaky test	[18313](https://github.com/ClickHouse/ClickHouse/pull/18313)	by @Enmk
  * Fixes in ODBC dictionary reload and ODBC bridge reachability	[18278](https://github.com/ClickHouse/ClickHouse/pull/18278)	by @traceon
  * OPTIMIZE DEDUPLICATE BY COLUMNS	[17846](https://github.com/ClickHouse/ClickHouse/pull/17846)	by @Enmk
  * Merging TestFlows requirements for AES encryption functions.	[18221](https://github.com/ClickHouse/ClickHouse/pull/18221)	by @vzakaznikov
  * Updating TestFlows version to the latest 1.6.72	[18208](https://github.com/ClickHouse/ClickHouse/pull/18208)	by @vzakaznikov
  * DETACH TABLE PERMANENTLY	[17642](https://github.com/ClickHouse/ClickHouse/pull/17642)	by @filimonov
  * Fixed `std::out_of_range: basic_string` in S3 URL parsing	[18059](https://github.com/ClickHouse/ClickHouse/pull/18059)	by @excitoon
  * kafka test_premature_flush_on_eof flap	[18000](https://github.com/ClickHouse/ClickHouse/pull/18000)	by @filimonov
  * Decrease log verbosity of disconnecting clients	[18005](https://github.com/ClickHouse/ClickHouse/pull/18005)	by @filimonov
  * dict notes	[17864](https://github.com/ClickHouse/ClickHouse/pull/17864)	by @filimonov
  * Added proper authentication for S3 client	[16856](https://github.com/ClickHouse/ClickHouse/pull/16856)	by @excitoon
  * Date vs DateTime64 comparison	[17895](https://github.com/ClickHouse/ClickHouse/pull/17895)	by @Enmk
  * Fixed comparison of DateTime64 with different scales	[16952](https://github.com/ClickHouse/ClickHouse/pull/16952)	by @Enmk
  * Updating TestFlows README.md to include "How To Debug Why Test Failed" section.	[17808](https://github.com/ClickHouse/ClickHouse/pull/17808)	by @vzakaznikov
  * Attempt to use IOStream in AWS SDK	[17794](https://github.com/ClickHouse/ClickHouse/pull/17794)	by @excitoon
  * Document JSONAsString	[17467](https://github.com/ClickHouse/ClickHouse/pull/17467)	by @filimonov
  * add a note for copier docs	[17468](https://github.com/ClickHouse/ClickHouse/pull/17468)	by @filimonov
  * Fix CMake generation and build for native Xcode and AppleClang	[17501](https://github.com/ClickHouse/ClickHouse/pull/17501)	by @traceon
  * Allow different types in avgWeighted. Allow avg and avgWeighed to operate on extended integral types.	[15419](https://github.com/ClickHouse/ClickHouse/pull/15419)	by @myrrc
  * Update cctz to the latest master, update tzdb to 2020d.	[17204](https://github.com/ClickHouse/ClickHouse/pull/17204)	by @filimonov
  * execute_merges_on_single_replica	[16424](https://github.com/ClickHouse/ClickHouse/pull/16424)	by @filimonov
  * Fixing unstable test in tests/testflows/ldap/external_user_directory/tests/authentications.py	[17161](https://github.com/ClickHouse/ClickHouse/pull/17161)	by @vzakaznikov
  * Fixed wrong result in big integers (128, 256 bit) when casting from double to int64_t.	[16986](https://github.com/ClickHouse/ClickHouse/pull/16986)	by @myrrc
  * Fix ROCKSDB_ERROR value	[17047](https://github.com/ClickHouse/ClickHouse/pull/17047)	by @traceon
  * Reresolve the IP of the `format_avro_schema_registry_url` in case of errors. 	[16985](https://github.com/ClickHouse/ClickHouse/pull/16985)	by @filimonov
  * Install script should always create subdirs in config folders. 	[16936](https://github.com/ClickHouse/ClickHouse/pull/16936)	by @filimonov
  * SNI for tcp secure	[16938](https://github.com/ClickHouse/ClickHouse/pull/16938)	by @filimonov
  * Fix for issue #16862	[16865](https://github.com/ClickHouse/ClickHouse/pull/16865)	by @filimonov
  * Backport #16865 to 20.3.	[16927](https://github.com/ClickHouse/ClickHouse/pull/16927)	by @filimonov
  * Remove timeSeriesGroupRateSum from docs	[16901](https://github.com/ClickHouse/ClickHouse/pull/16901)	by @filimonov
  * `ALTER UPDATE/DELETE ... IN PARTITION` with partition pruning in `ReplicatedMergeTree`	[13403](https://github.com/ClickHouse/ClickHouse/pull/13403)	by @excitoon
  * Create adding_test_queries.md	[16822](https://github.com/ClickHouse/ClickHouse/pull/16822)	by @filimonov
  * Test for the issue #12615	[16762](https://github.com/ClickHouse/ClickHouse/pull/16762)	by @filimonov
  * Update clickhouse-copier.md	[16663](https://github.com/ClickHouse/ClickHouse/pull/16663)	by @filimonov
  * Remove redundant diagnostics and fixed `test_jbod_overflow`	[16411](https://github.com/ClickHouse/ClickHouse/pull/16411)	by @excitoon
  * Update date-time-functions.md	[16549](https://github.com/ClickHouse/ClickHouse/pull/16549)	by @filimonov
  * Fixing the inability to deserialize AVRO into table if it contains LowCardinality columns	[16521](https://github.com/ClickHouse/ClickHouse/pull/16521)	by @myrrc
  * docker: clickhouse-server on the top of alpine	[16479](https://github.com/ClickHouse/ClickHouse/pull/16479)	by @filimonov
  * Update other-functions.md	[16480](https://github.com/ClickHouse/ClickHouse/pull/16480)	by @filimonov
  * Fix typos reported by codespell	[16425](https://github.com/ClickHouse/ClickHouse/pull/16425)	by @filimonov
  * Fix LDAP tests by grabbing log size after container is stopped	[16440](https://github.com/ClickHouse/ClickHouse/pull/16440)	by @vzakaznikov
  * Fixed flappy `test_multiple_disks`	[16235](https://github.com/ClickHouse/ClickHouse/pull/16235)	by @excitoon
  * clickhouse-local can work without tmp directory	[16280](https://github.com/ClickHouse/ClickHouse/pull/16280)	by @filimonov
  * Fixing another issue in LDAP tests	[16365](https://github.com/ClickHouse/ClickHouse/pull/16365)	by @vzakaznikov
  * Fixing fails in LDAP external user directory tests.	[16363](https://github.com/ClickHouse/ClickHouse/pull/16363)	by @vzakaznikov
  * Add a log message after an access storage is added	[16249](https://github.com/ClickHouse/ClickHouse/pull/16249)	by @traceon
  * Add setTemporaryStorage to clickhouse-local to make OPTIMIZE work	[16192](https://github.com/ClickHouse/ClickHouse/pull/16192)	by @filimonov
  * Fixing tests/queries/0_stateless/01446_json_strings_each_row.sql test	[16247](https://github.com/ClickHouse/ClickHouse/pull/16247)	by @vzakaznikov
  * encrypt and decrypt functions	[11844](https://github.com/ClickHouse/ClickHouse/pull/11844)	by @Enmk
  * Added `disable_merges` option for volumes in multi-disk configuration	[13956](https://github.com/ClickHouse/ClickHouse/pull/13956)	by @excitoon
  * Add LDAP user directory support for locally non-existent users	[12736](https://github.com/ClickHouse/ClickHouse/pull/12736)	by @traceon
  * Fixing arrayIndex functions when right operand is LC but left is not	[16038](https://github.com/ClickHouse/ClickHouse/pull/16038)	by @myrrc
  * ProtobufSingle format	[15199](https://github.com/ClickHouse/ClickHouse/pull/15199)	by @filimonov
  * Fix the bug when NOTHING_TO_DO events wrongly increment count_no_work_done	[15987](https://github.com/ClickHouse/ClickHouse/pull/15987)	by @filimonov
  * Better initialization of S3 storage	[15646](https://github.com/ClickHouse/ClickHouse/pull/15646)	by @excitoon
  * fix flap in no_holes_when_write_suffix_failed	[15757](https://github.com/ClickHouse/ClickHouse/pull/15757)	by @filimonov
  * Mention core_dump size limit in docs	[15416](https://github.com/ClickHouse/ClickHouse/pull/15416)	by @filimonov
  * Fixed compression in S3 storage	[15376](https://github.com/ClickHouse/ClickHouse/pull/15376)	by @excitoon
  * Fixing options' names' links in cmake docs generator	[15410](https://github.com/ClickHouse/ClickHouse/pull/15410)	by @myrrc
  * CMake flags reference generator, the guide for adding new options, and the attempt to correct the existing options	[14711](https://github.com/ClickHouse/ClickHouse/pull/14711)	by @myrrc
  * Better debug message from MergeTreeDataSelectExecutor	[15169](https://github.com/ClickHouse/ClickHouse/pull/15169)	by @filimonov
  * Fixing tests/integration/test_distributed_over_live_view/test.py	[14892](https://github.com/ClickHouse/ClickHouse/pull/14892)	by @vzakaznikov
  * Fix enable_optimize_predicate_expression for finalizeAggregation	[14937](https://github.com/ClickHouse/ClickHouse/pull/14937)	by @filimonov
  * Update clickhouse-benchmark.md	[14803](https://github.com/ClickHouse/ClickHouse/pull/14803)	by @filimonov
  * Extracted the debug info generation for functions into a cmake option	[14657](https://github.com/ClickHouse/ClickHouse/pull/14657)	by @myrrc
  * Correct nullability checker for LowCardinality nested columns	[14591](https://github.com/ClickHouse/ClickHouse/pull/14591)	by @myrrc
  * MySql datatypes dateTime64 and decimal	[11512](https://github.com/ClickHouse/ClickHouse/pull/11512)	by @Enmk
  * Fix a build for old some OS with old find	[14215](https://github.com/ClickHouse/ClickHouse/pull/14215)	by @filimonov
  * testflows: adding retry logic when bringing up docker-compose cluster	[14112](https://github.com/ClickHouse/ClickHouse/pull/14112)	by @vzakaznikov
  * tzdata improvements	[13648](https://github.com/ClickHouse/ClickHouse/pull/13648)	by @filimonov
  * Optimising has(), indexOf(), and countEqual() for Array(LowCardinality(T)) and constant right arguments	[12550](https://github.com/ClickHouse/ClickHouse/pull/12550)	by @myrrc
  * CI checks md file	[13615](https://github.com/ClickHouse/ClickHouse/pull/13615)	by @myrrc
  * Fixed flappy `test_multiple_disks::test_start_stop_moves`	[13759](https://github.com/ClickHouse/ClickHouse/pull/13759)	by @excitoon
  * Testflows LDAP module: adding missing certificates and dhparam.pem for openldap4	[13780](https://github.com/ClickHouse/ClickHouse/pull/13780)	by @vzakaznikov
  * Updating LDAP user authentication suite to check that it works with RBAC	[13656](https://github.com/ClickHouse/ClickHouse/pull/13656)	by @vzakaznikov
  * testflows: increasing health-check timeouts for clickhouse nodes	[13612](https://github.com/ClickHouse/ClickHouse/pull/13612)	by @vzakaznikov
  * Removed `-DENABLE_CURL_CLIENT` for `contrib/aws`	[13628](https://github.com/ClickHouse/ClickHouse/pull/13628)	by @excitoon
  * Proper remote host checking in S3 redirects	[13404](https://github.com/ClickHouse/ClickHouse/pull/13404)	by @excitoon
  * Fix for test_kafka_flush_by_block_size after rdkafka 1.5	[13285](https://github.com/ClickHouse/ClickHouse/pull/13285)	by @filimonov
  * Keep original query_masking_rules.xml when performing fasttests	[13382](https://github.com/ClickHouse/ClickHouse/pull/13382)	by @Enmk
  * Applying LDAP authentication test fixes	[13310](https://github.com/ClickHouse/ClickHouse/pull/13310)	by @vzakaznikov
  * Add test for macros usage in some kafka settings.	[13103](https://github.com/ClickHouse/ClickHouse/pull/13103)	by @filimonov
  * Volumes related refactorings	[12670](https://github.com/ClickHouse/ClickHouse/pull/12670)	by @excitoon
  * Small fixes to the RBAC SRS	[13152](https://github.com/ClickHouse/ClickHouse/pull/13152)	by @vzakaznikov
  * Fixing 00960_live_view_watch_events_live.py test	[13108](https://github.com/ClickHouse/ClickHouse/pull/13108)	by @vzakaznikov
  * Adding RBAC syntax tests.	[12642](https://github.com/ClickHouse/ClickHouse/pull/12642)	by @vzakaznikov
  * Adding extra xfails for some ldap tests.	[13054](https://github.com/ClickHouse/ClickHouse/pull/13054)	by @vzakaznikov
  * increasing timeouts in testflows tests	[12949](https://github.com/ClickHouse/ClickHouse/pull/12949)	by @vzakaznikov
  * Updated broken link in `asynchronous_metric_log.md`	[12766](https://github.com/ClickHouse/ClickHouse/pull/12766)	by @excitoon
  * Add LDAP authentication support	[11234](https://github.com/ClickHouse/ClickHouse/pull/11234)	by @traceon
  * Retain existing config.d/query_masking_rules.xml from server package	[12526](https://github.com/ClickHouse/ClickHouse/pull/12526)	by @Enmk
  * Fixing race condition in live view tables which could cause data duplication and live view tests	[12519](https://github.com/ClickHouse/ClickHouse/pull/12519)	by @vzakaznikov
  * Separated `AWSAuthV4Signer` into different logger, removed "AWSClient: AWSClient"	[12320](https://github.com/ClickHouse/ClickHouse/pull/12320)	by @excitoon
  * `min_bytes_for_seek` setting for `DiskS3`	[12434](https://github.com/ClickHouse/ClickHouse/pull/12434)	by @excitoon
  * Resolve #12098	[12397](https://github.com/ClickHouse/ClickHouse/pull/12397)	by @myrrc
  * Increasing default timeout for live view tests from 20 sec to 120 sec	[12416](https://github.com/ClickHouse/ClickHouse/pull/12416)	by @vzakaznikov
  * Backport #12120 to 20.4	[12396](https://github.com/ClickHouse/ClickHouse/pull/12396)	by @filimonov
  * Backport #12120 to 20.3	[12395](https://github.com/ClickHouse/ClickHouse/pull/12395)	by @filimonov
  * Implemented single part uploads for DiskS3	[12026](https://github.com/ClickHouse/ClickHouse/pull/12026)	by @excitoon
  * Adding a simple example of using TestFlows.	[12090](https://github.com/ClickHouse/ClickHouse/pull/12090)	by @vzakaznikov
  * Tests for fixed issues #10846, #7347, #3767	[12193](https://github.com/ClickHouse/ClickHouse/pull/12193)	by @filimonov
  * Add query context for system logs and to Buffer	[12120](https://github.com/ClickHouse/ClickHouse/pull/12120)	by @filimonov
  * Switched paths in S3 metadata to relative	[11892](https://github.com/ClickHouse/ClickHouse/pull/11892)	by @excitoon
  * ILIKE operator	[12125](https://github.com/ClickHouse/ClickHouse/pull/12125)	by @myrrc
  * Test for input_format_allow_errors_num in CSV	[12105](https://github.com/ClickHouse/ClickHouse/pull/12105)	by @filimonov
  * max_rows_to_read remove note about 'applied on each thread separately'	[12070](https://github.com/ClickHouse/ClickHouse/pull/12070)	by @filimonov
  * Moved useless S3 logging to TRACE level	[12067](https://github.com/ClickHouse/ClickHouse/pull/12067)	by @excitoon
  * Moves task shall be started if new storage policy needs them	[11893](https://github.com/ClickHouse/ClickHouse/pull/11893)	by @excitoon
  * Kafka work with formats based on PeekableReadBuffer and other improvements	[11599](https://github.com/ClickHouse/ClickHouse/pull/11599)	by @filimonov
  * Remove note about experimental from skipping indexes docs	[11704](https://github.com/ClickHouse/ClickHouse/pull/11704)	by @filimonov
  * extractAllGroupsHorizontal and extractAllGroupsVertical	[11554](https://github.com/ClickHouse/ClickHouse/pull/11554)	by @Enmk
  * Fix corner case (only) for exit code overflow	[11601](https://github.com/ClickHouse/ClickHouse/pull/11601)	by @filimonov
  * Adding support for PREWHERE in live view tables.	[11495](https://github.com/ClickHouse/ClickHouse/pull/11495)	by @vzakaznikov
  * backport #9884 to 20.3	[11552](https://github.com/ClickHouse/ClickHouse/pull/11552)	by @filimonov
  * 20.1 kafka backports	[11519](https://github.com/ClickHouse/ClickHouse/pull/11519)	by @filimonov
  * 20.3 kafka backports	[11520](https://github.com/ClickHouse/ClickHouse/pull/11520)	by @filimonov
  * Fixed using nullptr source and dest buffers in codecs, fixed test	[11471](https://github.com/ClickHouse/ClickHouse/pull/11471)	by @Enmk
  * Added tests for improved S3 URL parsing	[11174](https://github.com/ClickHouse/ClickHouse/pull/11174)	by @excitoon
  * Better settings for Kafka	[11388](https://github.com/ClickHouse/ClickHouse/pull/11388)	by @filimonov
  * Fixed geohashesInBox argument range	[11403](https://github.com/ClickHouse/ClickHouse/pull/11403)	by @Enmk
  * Virtual columns for Kafka headers	[11283](https://github.com/ClickHouse/ClickHouse/pull/11283)	by @filimonov
  * Add _timestamp_ms virtual columns to Kafka engine	[11260](https://github.com/ClickHouse/ClickHouse/pull/11260)	by @filimonov
  * Kafka clientid (finishing #11073)	[11252](https://github.com/ClickHouse/ClickHouse/pull/11252)	by @filimonov
  * Update librdkafka to version 1.4.2	[11256](https://github.com/ClickHouse/ClickHouse/pull/11256)	by @filimonov
  * Add libsasl2-dev and heimdal-multidev in CI Docker image	[11310](https://github.com/ClickHouse/ClickHouse/pull/11310)	by @traceon
  * Add prefix & facility to librdkafka logs	[11261](https://github.com/ClickHouse/ClickHouse/pull/11261)	by @filimonov
  * Fixed reschedule issue in Kafka	[11149](https://github.com/ClickHouse/ClickHouse/pull/11149)	by @filimonov
  * {to,from}UnixTimestamp64{Milli,Micro,Nano} functions	[10923](https://github.com/ClickHouse/ClickHouse/pull/10923)	by @Enmk
  * Fixed S3 globbing which could fail in case of more than 1000 keys and some backends	[11179](https://github.com/ClickHouse/ClickHouse/pull/11179)	by @excitoon
  * Fix for the hang during deletion of engine=Kafka (one more time)	[11145](https://github.com/ClickHouse/ClickHouse/pull/11145)	by @filimonov
  * Revert "Disable some flappy tests"	[8840](https://github.com/ClickHouse/ClickHouse/pull/8840)	by @excitoon
  * Fixed parsing of S3 URL	[11036](https://github.com/ClickHouse/ClickHouse/pull/11036)	by @excitoon
  * Fixed parseDateTime64BestEffort implementation	[11038](https://github.com/ClickHouse/ClickHouse/pull/11038)	by @Enmk
  * Fixes the potential missed data during termination of Kafka engine table	[11048](https://github.com/ClickHouse/ClickHouse/pull/11048)	by @filimonov
  * Added `move_ttl_info` to `system.parts`	[10591](https://github.com/ClickHouse/ClickHouse/pull/10591)	by @excitoon
  * Fixed link to external dictionaries	[11020](https://github.com/ClickHouse/ClickHouse/pull/11020)	by @excitoon
  * Fixing 00979_live_view_watch_continuous_aggregates test	[11024](https://github.com/ClickHouse/ClickHouse/pull/11024)	by @vzakaznikov
  * Fix for the hang during deletion of engine=Kafka	[10910](https://github.com/ClickHouse/ClickHouse/pull/10910)	by @filimonov
  * Adding support for ALTER RENAME COLUMN query to Distributed table engine	[10727](https://github.com/ClickHouse/ClickHouse/pull/10727)	by @vzakaznikov
  * Add OpenLDAP third-party library integration (system or contrib/build)	[10861](https://github.com/ClickHouse/ClickHouse/pull/10861)	by @traceon
  * function toStartOfSecond(DateTime64) -> DateTime64	[10722](https://github.com/ClickHouse/ClickHouse/pull/10722)	by @Enmk
  * Fixed DateLUTImpl constructors to avoid accidental copying	[10809](https://github.com/ClickHouse/ClickHouse/pull/10809)	by @Enmk
  * Fixed handling condition variable for synchronous mutations	[10588](https://github.com/ClickHouse/ClickHouse/pull/10588)	by @excitoon
  * Fixing and re-enabling 00979_live_view_watch_continuous_aggregates.py test.	[10658](https://github.com/ClickHouse/ClickHouse/pull/10658)	by @vzakaznikov
  * extractAllGroups(haystack, re_needle) function	[10534](https://github.com/ClickHouse/ClickHouse/pull/10534)	by @Enmk
  * Cleaned up AppleClang version check	[10708](https://github.com/ClickHouse/ClickHouse/pull/10708)	by @traceon
  * Blind fix for AppleClang version and char8_t support check	[10705](https://github.com/ClickHouse/ClickHouse/pull/10705)	by @traceon
  * Trying to fix tests/queries/0_stateless/01246_insert_into_watch_live_view.py test	[10670](https://github.com/ClickHouse/ClickHouse/pull/10670)	by @vzakaznikov
  * Fixing hard coded timeouts in new live view tests.	[10604](https://github.com/ClickHouse/ClickHouse/pull/10604)	by @vzakaznikov
  * Xcode generator build fix	[10541](https://github.com/ClickHouse/ClickHouse/pull/10541)	by @traceon
  * Fixed comparing DateTime64 in WHERE against String value	[10560](https://github.com/ClickHouse/ClickHouse/pull/10560)	by @Enmk
  * Increasing timeout when opening a client in tests/0_stateless/helpers/client.py	[10599](https://github.com/ClickHouse/ClickHouse/pull/10599)	by @vzakaznikov
  * Adding support for INSERT INTO table WATCH query to build streaming systems using LIVE VIEW tables	[10498](https://github.com/ClickHouse/ClickHouse/pull/10498)	by @vzakaznikov
  * add CA certificates to clickhouse-server docker image	[10476](https://github.com/ClickHouse/ClickHouse/pull/10476)	by @filimonov
  * Allowed to alter column in non-modifying data mode when the same type is specified	[10382](https://github.com/ClickHouse/ClickHouse/pull/10382)	by @excitoon
  * Fixing 00964_live_view_watch_events_heartbeat.py test	[10356](https://github.com/ClickHouse/ClickHouse/pull/10356)	by @vzakaznikov
  * Initial support for live view tables over distributed	[10179](https://github.com/ClickHouse/ClickHouse/pull/10179)	by @vzakaznikov
  * Splitting string into Alpha-Num tokens with SIMD intrinsics.	[9968](https://github.com/ClickHouse/ClickHouse/pull/9968)	by @Enmk
  * clickhouse-docker-util	[10151](https://github.com/ClickHouse/ClickHouse/pull/10151)	by @filimonov
  * allow_nondeterministic_mutations	[10186](https://github.com/ClickHouse/ClickHouse/pull/10186)	by @filimonov
  * 19.16 backports	[10156](https://github.com/ClickHouse/ClickHouse/pull/10156)	by @excitoon
  * Backport #9884 to 19.16 [Kafka retry commits on failure]	[10144](https://github.com/ClickHouse/ClickHouse/pull/10144)	by @excitoon
  * Fixing 00964_live_view_watch_events_heartbeat.py test to avoid race condition	[9944](https://github.com/ClickHouse/ClickHouse/pull/9944)	by @vzakaznikov
  * Fixed `DeleteOnDestroy` logic in `ATTACH PART` and added few tests	[9410](https://github.com/ClickHouse/ClickHouse/pull/9410)	by @excitoon
  * Kafka retry commits on failure	[9884](https://github.com/ClickHouse/ClickHouse/pull/9884)	by @filimonov
  * Date time various timezone fixes	[9574](https://github.com/ClickHouse/ClickHouse/pull/9574)	by @Enmk
  * Added MATERIALIZE TTL IN PARTITION	[9581](https://github.com/ClickHouse/ClickHouse/pull/9581)	by @excitoon
  * Try newer version of odbc driver	[9484](https://github.com/ClickHouse/ClickHouse/pull/9484)	by @filimonov
  * Date time formatting tests	[9567](https://github.com/ClickHouse/ClickHouse/pull/9567)	by @Enmk
  * Fixed wrong log messages about missing default disk or policy	[9530](https://github.com/ClickHouse/ClickHouse/pull/9530)	by @excitoon
  * Added reloading storage configuration from configuration file	[8594](https://github.com/ClickHouse/ClickHouse/pull/8594)	by @excitoon
  * Kafka exceptions from destructors	[9513](https://github.com/ClickHouse/ClickHouse/pull/9513)	by @filimonov
  *  Set X-ClickHouse-Timezone HTTP response header to the server's timezone	[9493](https://github.com/ClickHouse/ClickHouse/pull/9493)	by @traceon
  * Backport 8549 to 19.16 [Do not allow to merge data moving it against storage policy volume order]	[9496](https://github.com/ClickHouse/ClickHouse/pull/9496)	by @excitoon
  * Backport 8549 to 19.16 [Do not allow to merge data moving it against storage policy volume order]	[9486](https://github.com/ClickHouse/ClickHouse/pull/9486)	by @excitoon
  * Little typo fixed	[9397](https://github.com/ClickHouse/ClickHouse/pull/9397)	by @excitoon
  * Added a check for storage policy in `cloneAndLoadDataPartOnSameDisk()`	[9383](https://github.com/ClickHouse/ClickHouse/pull/9383)	by @excitoon
  * adjust dockerfile used in tests to allow odbc roundtrip	[9348](https://github.com/ClickHouse/ClickHouse/pull/9348)	by @filimonov
  * Pass TTL rule info with block to other replicas	[8598](https://github.com/ClickHouse/ClickHouse/pull/8598)	by @excitoon
  * key and timestamp in Kafka producer	[8969](https://github.com/ClickHouse/ClickHouse/pull/8969)	by @filimonov
  * 19.17 backports	[8992](https://github.com/ClickHouse/ClickHouse/pull/8992)	by @filimonov
  * 20.1 few backports	[8993](https://github.com/ClickHouse/ClickHouse/pull/8993)	by @filimonov
  * Fixed buffer overflow on decoding small sequences with Gorilla and DoubleDelta	[9028](https://github.com/ClickHouse/ClickHouse/pull/9028)	by @Enmk
  * 19.16 few fixes backported	[8991](https://github.com/ClickHouse/ClickHouse/pull/8991)	by @filimonov
  * IN with function result	[5342](https://github.com/ClickHouse/ClickHouse/pull/5342)	by @Enmk
  * Added globs/wildcards for s3	[8851](https://github.com/ClickHouse/ClickHouse/pull/8851)	by @excitoon
  * Kafka fixes part2	[8917](https://github.com/ClickHouse/ClickHouse/pull/8917)	by @filimonov
  * Fixed `StorageInput::StorageInput` a little bit	[8850](https://github.com/ClickHouse/ClickHouse/pull/8850)	by @excitoon
  * "Atomically" remove parts on destroy.	[8402](https://github.com/ClickHouse/ClickHouse/pull/8402)	by @excitoon
  * Reworking fix for issue 7878 (version 2)	[8788](https://github.com/ClickHouse/ClickHouse/pull/8788)	by @vzakaznikov
  * Fixed bug with `MergeTreeReadPool`	[8791](https://github.com/ClickHouse/ClickHouse/pull/8791)	by @excitoon
  * Set X-ClickHouse-Format HTTP response header to the format name	[8769](https://github.com/ClickHouse/ClickHouse/pull/8769)	by @traceon
  * Fix issue #7878	[8766](https://github.com/ClickHouse/ClickHouse/pull/8766)	by @vzakaznikov
  * Kafka fixes backport 19 17	[8763](https://github.com/ClickHouse/ClickHouse/pull/8763)	by @filimonov
  * Test for incremental filling with feedback	[8744](https://github.com/ClickHouse/ClickHouse/pull/8744)	by @filimonov
  * Fixed `ALTER MODIFY TTL`	[8422](https://github.com/ClickHouse/ClickHouse/pull/8422)	by @excitoon
  * Fixed `MergeTreeData::areBackgroundMovesNeeded` according to move TTL feature	[8672](https://github.com/ClickHouse/ClickHouse/pull/8672)	by @excitoon
  * Fixed a bug with double move which corrupt original part	[8680](https://github.com/ClickHouse/ClickHouse/pull/8680)	by @excitoon
  * typo fix	[8666](https://github.com/ClickHouse/ClickHouse/pull/8666)	by @filimonov
  * Fixed deduplication issues in more `test_multiple_disks` tests	[8671](https://github.com/ClickHouse/ClickHouse/pull/8671)	by @excitoon
  * Fixed deduplication issue in `test_multiple_disks::test_concurrent_alter_move`	[8642](https://github.com/ClickHouse/ClickHouse/pull/8642)	by @excitoon
  * Allow to change `storage_policy` to not less rich one	[8107](https://github.com/ClickHouse/ClickHouse/pull/8107)	by @excitoon
  * Do not allow to merge data moving it against storage policy volume order	[8549](https://github.com/ClickHouse/ClickHouse/pull/8549)	by @excitoon
  * Fixed codec performance test	[8615](https://github.com/ClickHouse/ClickHouse/pull/8615)	by @Enmk
  * fix printing changelog with non-ascii chars	[8611](https://github.com/ClickHouse/ClickHouse/pull/8611)	by @filimonov
  * Increased number of rows to make tests results noticeable.	[8574](https://github.com/ClickHouse/ClickHouse/pull/8574)	by @Enmk
  * Live View storage engine refactoring of getNewBlocks() and writeIntoLiveView() methods.	[8519](https://github.com/ClickHouse/ClickHouse/pull/8519)	by @vzakaznikov
  * make_changelog.py::process_unknown_commits unicode fix	[8565](https://github.com/ClickHouse/ClickHouse/pull/8565)	by @filimonov
  * Added creation of user and working directory to Arch Linux install script	[8534](https://github.com/ClickHouse/ClickHouse/pull/8534)	by @excitoon
  * Gorilla and doubledelta performance	[8019](https://github.com/ClickHouse/ClickHouse/pull/8019)	by @Enmk
  * Fixed build of `openssl` with `make`	[8528](https://github.com/ClickHouse/ClickHouse/pull/8528)	by @excitoon
  * Improved extracting of GCC version.	[8516](https://github.com/ClickHouse/ClickHouse/pull/8516)	by @excitoon
  * Live view support for subquery	[7792](https://github.com/ClickHouse/ClickHouse/pull/7792)	by @vzakaznikov

## 2019
  * Improved check for parts on different disks	[8440](https://github.com/ClickHouse/ClickHouse/pull/8440)	by @excitoon
  * Fixed ALTER TTL for replicated tables	[8318](https://github.com/ClickHouse/ClickHouse/pull/8318)	by @excitoon
  * Added check for valid destination in a move TTL rule	[8410](https://github.com/ClickHouse/ClickHouse/pull/8410)	by @excitoon
  * Check extra parts of `MergeTree` at different disks, in order to not allow to miss data parts at undefined disks	[8118](https://github.com/ClickHouse/ClickHouse/pull/8118)	by @excitoon
  * Added `test_ttl_move::test_ttls_do_not_work_after_alter` test	[8407](https://github.com/ClickHouse/ClickHouse/pull/8407)	by @excitoon
  * Perf tests for all supported codecs against Float64 and UInt64.	[8349](https://github.com/ClickHouse/ClickHouse/pull/8349)	by @Enmk
  * Fixed type check in toDateTime64	[8375](https://github.com/ClickHouse/ClickHouse/pull/8375)	by @Enmk
  * Handling error from clock_gettime properly	[8291](https://github.com/ClickHouse/ClickHouse/pull/8291)	by @Enmk
  * now64() tests	[8274](https://github.com/ClickHouse/ClickHouse/pull/8274)	by @Enmk
  * Fixed crash in now64() when it argument is a result of function call.	[8270](https://github.com/ClickHouse/ClickHouse/pull/8270)	by @Enmk
  * Added english documentation for extended TTL syntax	[8261](https://github.com/ClickHouse/ClickHouse/pull/8261)	by @excitoon
  * Kafka fixes backport 19.16	[8249](https://github.com/ClickHouse/ClickHouse/pull/8249)	by @filimonov
  * Added documentation for extended TTL syntax	[8059](https://github.com/ClickHouse/ClickHouse/pull/8059)	by @excitoon
  * Init query context for Kafka to make subqueries work	[8197](https://github.com/ClickHouse/ClickHouse/pull/8197)	by @filimonov
  * Improved performance of max(), min(), argMin(), argMax() for DateTime64	[8199](https://github.com/ClickHouse/ClickHouse/pull/8199)	by @Enmk
  * Fixed `test_multiple_disks::test_kill_while_insert` a little	[8135](https://github.com/ClickHouse/ClickHouse/pull/8135)	by @excitoon
  * Test for issue #5142	[8190](https://github.com/ClickHouse/ClickHouse/pull/8190)	by @filimonov
  * Better logging in background move task.	[8192](https://github.com/ClickHouse/ClickHouse/pull/8192)	by @excitoon
  * Fixed metrics in `BackgroundProcessingPool`	[8194](https://github.com/ClickHouse/ClickHouse/pull/8194)	by @excitoon
  * Fixed flapping `test_ttl_move::test_moves_after_merges_work`	[8173](https://github.com/ClickHouse/ClickHouse/pull/8173)	by @excitoon
  * DateTime64 data type	[7170](https://github.com/ClickHouse/ClickHouse/pull/7170)	by @Enmk
  * Move parts between storage volumes according to configured TTL expressions	[8140](https://github.com/ClickHouse/ClickHouse/pull/8140)	by @excitoon
  * Fixed linker searching logic	[8139](https://github.com/ClickHouse/ClickHouse/pull/8139)	by @excitoon
  * Move parts between storage volumes according to configured TTL expressions	[7420](https://github.com/ClickHouse/ClickHouse/pull/7420)	by @excitoon
  * Fixed a typo	[8134](https://github.com/ClickHouse/ClickHouse/pull/8134)	by @excitoon
  * Better linking	[8115](https://github.com/ClickHouse/ClickHouse/pull/8115)	by @excitoon
  * FIxed behavior with ALTER MOVE ran immediately after merge finish moves superpart of specified	[8104](https://github.com/ClickHouse/ClickHouse/pull/8104)	by @excitoon
  * Ignore redundant copies of parts after move and restart	[7810](https://github.com/ClickHouse/ClickHouse/pull/7810)	by @excitoon
  * Removed `localtime` from `HTTPDictionarySource::getUpdateFieldAndDate…	[8042](https://github.com/ClickHouse/ClickHouse/pull/8042)	by @excitoon
  * Added `-D LINKER_NAME=lld` to instruction to sanitizers.	[8025](https://github.com/ClickHouse/ClickHouse/pull/8025)	by @excitoon
  * Kafka fixes	[8016](https://github.com/ClickHouse/ClickHouse/pull/8016)	by @filimonov
  * Added information about paths to `system.merges`.	[8043](https://github.com/ClickHouse/ClickHouse/pull/8043)	by @excitoon
  * Authentication in S3 table function and storage	[7623](https://github.com/ClickHouse/ClickHouse/pull/7623)	by @excitoon
  * Fix build with Poco Redis	[7835](https://github.com/ClickHouse/ClickHouse/pull/7835)	by @filimonov
  * [wip] attempt to improve kafka parsing performance 	[7935](https://github.com/ClickHouse/ClickHouse/pull/7935)	by @filimonov
  * ALTER to LowCardinality was leading to segfault on empty parts	[7985](https://github.com/ClickHouse/ClickHouse/pull/7985)	by @filimonov
  * Separated pool for background moves	[7670](https://github.com/ClickHouse/ClickHouse/pull/7670)	by @excitoon
  * Removed check for using `Date` or `DateTime` column from TTL expressions	[7920](https://github.com/ClickHouse/ClickHouse/pull/7920)	by @excitoon
  * Added disk info to `system.detached_parts`	[7833](https://github.com/ClickHouse/ClickHouse/pull/7833)	by @excitoon
  * Fixed errors with space reservation introduced in #7558 and #7602	[7873](https://github.com/ClickHouse/ClickHouse/pull/7873)	by @excitoon
  * Disable DwarfFDECache in libunwind	[7838](https://github.com/ClickHouse/ClickHouse/pull/7838)	by @filimonov
  * odbc table function now respects external_table_functions_use_nulls	[7506](https://github.com/ClickHouse/ClickHouse/pull/7506)	by @Enmk
  * Fixed bug with `keep_free_space_ratio` not being read from disks configuration	[7645](https://github.com/ClickHouse/ClickHouse/pull/7645)	by @excitoon
  * Fixed exception in case of using 1 argument while defining S3, URL and HDFS storages	[7618](https://github.com/ClickHouse/ClickHouse/pull/7618)	by @excitoon
  * Fixed comment in configuration of `test_multiple_disks`.	[7636](https://github.com/ClickHouse/ClickHouse/pull/7636)	by @excitoon
  * Fixed comment in configuration of `test_multiple_disks`.	[7636](https://github.com/ClickHouse/ClickHouse/pull/7636)	by @excitoon
  * Made `MergeTreeData::cloneAndLoadDataPart` only work for the same disk.	[7602](https://github.com/ClickHouse/ClickHouse/pull/7602)	by @excitoon
  * Made mutation to choose the same disk in `ReplicatedMergeTree`.	[7558](https://github.com/ClickHouse/ClickHouse/pull/7558)	by @excitoon
  * remove some obsolete notes about mutations	[7483](https://github.com/ClickHouse/ClickHouse/pull/7483)	by @filimonov
  * Add handling of SQL_TINYINT and SQL_BIGINT, fix SQL_FLOAT in ODBC Bridge	[7491](https://github.com/ClickHouse/ClickHouse/pull/7491)	by @traceon
  * Allowed to have some parts on destination disk or volume in MOVE PARTITION	[7434](https://github.com/ClickHouse/ClickHouse/pull/7434)	by @excitoon
  * Added integration test for #7414 (validation of `max_data_part_size_bytes`).	[7450](https://github.com/ClickHouse/ClickHouse/pull/7450)	by @excitoon
  * Fixed erroneous warning `max_data_part_size is too low`	[7423](https://github.com/ClickHouse/ClickHouse/pull/7423)	by @excitoon
  * Fixed NULL-values in nullable columns through ODBC-bridge	[7402](https://github.com/ClickHouse/ClickHouse/pull/7402)	by @Enmk
  * Fixing ThreadSanitizer data race error in the LIVE VIEW when accessing no_users_thread variable	[7353](https://github.com/ClickHouse/ClickHouse/pull/7353)	by @vzakaznikov
  * Fixing AddressSanitizer error in the LIVE VIEW getHeader() method.	[7271](https://github.com/ClickHouse/ClickHouse/pull/7271)	by @vzakaznikov
  * Fixed issue of using HTTP timeout as TCP timeout value.	[7351](https://github.com/ClickHouse/ClickHouse/pull/7351)	by @Enmk
  * Improved readability a little bit (`MergeTreeData::getActiveContainingPart`).	[7361](https://github.com/ClickHouse/ClickHouse/pull/7361)	by @excitoon
  * More performance test for Date and DateTime	[7332](https://github.com/ClickHouse/ClickHouse/pull/7332)	by @Enmk
  * A quick fix to resolve crash in LIVE VIEW table and re-enabling all LIVE VIEW tests.	[7201](https://github.com/ClickHouse/ClickHouse/pull/7201)	by @vzakaznikov
  * Minor doc fixes.	[7199](https://github.com/ClickHouse/ClickHouse/pull/7199)	by @excitoon
  * Fixed time calculation in `MergeTreeData`.	[7172](https://github.com/ClickHouse/ClickHouse/pull/7172)	by @excitoon
  * rpm: preserve existing configs on package upgrade	[7103](https://github.com/ClickHouse/ClickHouse/pull/7103)	by @filimonov
  * add obsoletes section to clickhouse-server spec	[7073](https://github.com/ClickHouse/ClickHouse/pull/7073)	by @filimonov
  * Adding performance test for huge pk (issue #6924)	[6980](https://github.com/ClickHouse/ClickHouse/pull/6980)	by @filimonov
  * performance test for lowcardinality array	[6930](https://github.com/ClickHouse/ClickHouse/pull/6930)	by @filimonov
  * Docs cleanup	[6859](https://github.com/ClickHouse/ClickHouse/pull/6859)	by @Enmk
  * Improvements for failover of Distributed queries	[6399](https://github.com/ClickHouse/ClickHouse/pull/6399)	by @Enmk
  * Better objects ownership in QueryMaskingRules	[6810](https://github.com/ClickHouse/ClickHouse/pull/6810)	by @filimonov
  * Fix some pvs reported issues	[6837](https://github.com/ClickHouse/ClickHouse/pull/6837)	by @filimonov
  * Ability to change history path by changing env	[6840](https://github.com/ClickHouse/ClickHouse/pull/6840)	by @filimonov
  * Query masking rules	[5710](https://github.com/ClickHouse/ClickHouse/pull/5710)	by @filimonov
  * Reduced children_mutex lock scope in IBlockInputStream	[6740](https://github.com/ClickHouse/ClickHouse/pull/6740)	by @Enmk
  * Fix live view no users thread	[6656](https://github.com/ClickHouse/ClickHouse/pull/6656)	by @vzakaznikov
  * Implemented hasTokenCaseInsensitive function	[6662](https://github.com/ClickHouse/ClickHouse/pull/6662)	by @Enmk
  * hasToken function implementation	[6596](https://github.com/ClickHouse/ClickHouse/pull/6596)	by @Enmk
  * remove doubled const TABLE_IS_READ_ONLY	[6566](https://github.com/ClickHouse/ClickHouse/pull/6566)	by @filimonov
  * Uninstrusive implementation of LIVE VIEW tables	[5541](https://github.com/ClickHouse/ClickHouse/pull/5541)	by @vzakaznikov
  * Fixed Gorilla encoding error on small sequences.	[6444](https://github.com/ClickHouse/ClickHouse/pull/6444)	by @Enmk
  * geohashesInbox(lon_min, lat_min, lon_max, lat_max, precision) function	[6127](https://github.com/ClickHouse/ClickHouse/pull/6127)	by @Enmk
  * Fixed Gorilla and DoubleDelta codec performance tests.	[6179](https://github.com/ClickHouse/ClickHouse/pull/6179)	by @Enmk
  * Docs enable optimize predicate expression	[6122](https://github.com/ClickHouse/ClickHouse/pull/6122)	by @filimonov
  * cmake - disallow switching ENABLE_MONGODB separate from ENABLE_POCO_MONGODB	[6077](https://github.com/ClickHouse/ClickHouse/pull/6077)	by @filimonov
  * configs for query masking rules tests	[6009](https://github.com/ClickHouse/ClickHouse/pull/6009)	by @filimonov
  * Fixed DoubleDelta encoding cases for random Int32 and Int64.	[5998](https://github.com/ClickHouse/ClickHouse/pull/5998)	by @Enmk
  * Fixed DoubleDelta codec edge case	[5824](https://github.com/ClickHouse/ClickHouse/pull/5824)	by @Enmk
  * fix macos build after #4828	[5830](https://github.com/ClickHouse/ClickHouse/pull/5830)	by @filimonov
  * fix build on macosx and gcc9	[5822](https://github.com/ClickHouse/ClickHouse/pull/5822)	by @filimonov
  * Gorilla column encoding	[5600](https://github.com/ClickHouse/ClickHouse/pull/5600)	by @Enmk
  * Small improvements for docs of Engine=Join	[5433](https://github.com/ClickHouse/ClickHouse/pull/5433)	by @filimonov
  * Changelog renames issues	[5428](https://github.com/ClickHouse/ClickHouse/pull/5428)	by @filimonov
  * more fixes for integration tests dockerfiles 	[5360](https://github.com/ClickHouse/ClickHouse/pull/5360)	by @filimonov
  * Several improvements in integration tests runner	[5340](https://github.com/ClickHouse/ClickHouse/pull/5340)	by @filimonov
  * Implementation of geohashEncode and geohashDecode functions;	[5003](https://github.com/ClickHouse/ClickHouse/pull/5003)	by @Enmk
  * IPv4 and IPv6 domain docs.	[5210](https://github.com/ClickHouse/ClickHouse/pull/5210)	by @Enmk
  * insert_sample_with_metadata small fixes in doc	[5121](https://github.com/ClickHouse/ClickHouse/pull/5121)	by @filimonov
  * add clickhouse-benchmark accepted stages names	[5006](https://github.com/ClickHouse/ClickHouse/pull/5006)	by @filimonov
  * Fixed test failures when running clickhouse-server on different host	[4713](https://github.com/ClickHouse/ClickHouse/pull/4713)	by @Enmk
  * Test runner script and corresponding Dockerfile and docker-compose.	[4347](https://github.com/ClickHouse/ClickHouse/pull/4347)	by @Enmk

## 2018
  * Added support of int-based types: Int\UInt(8,16,32), Date, DateTime f…	[3123](https://github.com/ClickHouse/ClickHouse/pull/3123)	by @Enmk
