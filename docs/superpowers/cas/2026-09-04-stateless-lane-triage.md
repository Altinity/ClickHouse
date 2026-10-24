---
description: 'Final triage of the local CA-s3 stateless lane run after the backend request-contract migration (11137 tests): every failure classified, the 404-scope fix confirmed at full scale, measured frequencies for the PUT-timeout and catalog-starvation items, and one new finding about a lease fence surfacing to synchronous client calls.'
sidebar_label: 'Stateless lane triage (2026-09-04)'
sidebar_position: 7
slug: /superpowers/cas/stateless-lane-triage-2026-09-04
title: 'CA-s3 stateless lane after the request-contract migration — final triage (2026-09-04)'
doc_type: 'reference'
---

# Task 24 run 2 — final triage of the CA-s3 stateless lane {#task-24-run-2-final-triage-of-the-ca-s3-stateless-lane}

Lane: `Stateless tests (amd_binary, cas s3 storage, parallel)`, binary `b70719e9896` (engine fixes
F1/F8/F4 + the 404 scope + merge #1), branch `cas-gc-rebuild`. Log: `build/test_task24_stateless.log`
(90583 lines). Started `2026-09-04T00:30:16Z`, ran the full suite to completion (not stopped early).

## Final counts {#final-counts}

From `ci/tmp/test_result.txt` and the log's own per-worker summaries (`Having N errors! M tests passed.
K tests skipped.`), and independently confirmed by counting `[ OK ]` / `[ FAIL ]` / `[ SKIPPED ]` /
`[ BROKEN ]` markers in the log:

| | count |
|---|---|
| ran (total) | 11137 |
| passed | 10721 |
| failed (FAIL + BROKEN) | 295 |
| skipped | 121 |
| BROKEN | 0 |
| wall time | 8561 s (~142 min) |

10721 + 295 + 121 = 11137, consistent. `ci/tmp/result_stateless_tests_amd_binary_cas_s3_storage_parallel.json`
carries `status: ERROR` with `"message": "Invalid status [PENDING] for exit code [1]"` — a praktika
harness-wrapper artifact (the job's own `results` array was never populated because the process exit
code from the full local run didn't match what the wrapper expected), not a test-content signal; the
authoritative per-test data is `ci/tmp/test_result.txt` and the log, both used above.

## Method {#method}

Parsed every `[ N / 11137 ] <name>: [ FAIL|BROKEN ] <time> sec.` marker in
`build/test_task24_stateless.log` and took the text between it and the next marker as that test's own
`Reason:`/stderr/stdout block (the harness serializes each worker's full per-test report as one
contiguous write, so this boundary is reliable — verified by spot-checking that every block ends in the
expected `Database: test_xxxxxxxx` trailer). Classified each of the 295 blocks by content. Verified two
apparent false positives from an early content-substring pass by reading full context and, in two cases,
the raw `clickhouse-server` log around the failure timestamp (`ci/tmp/var/log/clickhouse-server/clickhouse-server.log`,
4.6 GB / 24.8M lines for the whole run).

## Per-class counts {#per-class-counts}

| class | count | verdict |
|---|---:|---|
| A — stateful dataset absent locally (`test.hits`/`test.visits`/tpcds/tpch) | 236 | known, local-infra gap |
| PUT-timeout (`WriteBufferFromS3` Timeout on a CAS control-plane key) | 42 | known, BACKLOG `{#cas-s3-lane-put-timeout-logged-at-error}` |
| ignore-list (lane's known non-CAS entries) | 9 | known, not CAS |
| aux-zk-local-config-gap (auxiliary ZooKeeper chroot never created locally) | 4 | new class, local-infra gap (root-caused below) |
| mount-lease-fence (genuine CAS mount-lease loss under load) | 2 | NEW finding, root-caused below |
| catalog-starvation (`gave up at the policy deadline`) | 1 | known, BACKLOG `{#ref-catalog-cas-starvation}` |
| harness-hang-under-load (`clickhouse-obfuscator`, lldb dump) | 1 | the brief's named case, root-caused below |
| B — `AWSClient: Response status: 404` recurrence | **0** | **no recurrence — the 404-scope fix holds** |
| **total** | **295** | |

Sum check: 236 + 42 + 9 + 4 + 2 + 1 + 1 = 295.

**Class B is gone.** Zero occurrences of `AWSClient: Response status: 404` anywhere across all 295
failing blocks (grepped the full text of every block, not just the first match). Run 1 (190/11137
sampled, stopped early) saw 54/62 fails as class B; on the full run of 11137 tests it is zero. Commit
`6830b73af27` (the `Expect404ResponseScope` fix) is confirmed effective at full-suite scale.

## Root causes for the two counts the brief asked to be measured {#root-causes-for-the-two-counts-the-brief-asked-to-be-measured}

- **PUT-timeout: 42 occurrences** (BACKLOG `{#cas-s3-lane-put-timeout-logged-at-error}`) — this is the
  measured frequency for that BACKLOG item's "recorded item" note. Test list in the appendix.
- **Catalog starvation: 1 occurrence** (BACKLOG `{#ref-catalog-cas-starvation}`) — `01039_mergetree_exec_time`,
  `CREATE TABLE` gave up after 78.90 s (`CAS ref catalog 'cas_s3/cas/ref_catalog' update: gave up at the
  policy deadline after one or more attempt(s)`). Same test and failure shape the BACKLOG entry already
  cites (the entry's own RCA doc references this exact test and timing), so this is a repeat observation
  of the same tracked cause, not a new one.

## Server-restart / "server died" check {#server-restart-server-died-check}

Grepped the log for `Connection refused`, `Server died`, `STOP_TESTING_EXIT_CODE` and the harness's own
`Having N errors!` per-worker summaries. None of the 295 failures carry a `return code: 210`-class
harness abort or a "Server died" leaf; the log's final section (`Checking the hung queries: done`, `No
queries hung.`, `All tests have finished.`) shows the run completed normally with the same server process
throughout — no restart during the run.

## Timeouts {#timeouts}

Exactly one timeout-driven failure: `00092_obfuscator` (`Reason: Timeout! Killing process group`, 1200.03
sec — the harness's per-test ceiling), root-caused below. No other of the 295 failures carries a `Timeout!
Killing process group` reason; the PUT-timeout class's 42 tests are S3-client-side write timeouts (5 s
`attempt_timeout_ms`) surfacing on stderr, not harness-level test timeouts, so they are a distinct
mechanism from this one process-group kill.

## The two NEW / non-trivially-classified findings {#the-two-new-non-trivially-classified-findings}

### 1. `aux-zk-local-config-gap` — 4 tests: `02735_system_zookeeper_auxiliary`, {#1-aux-zk-local-config-gap-4-tests-02735-system-zookeeper-auxiliary}
`02442_auxiliary_zookeeper_endpoint_id`, `02735_system_zookeeper_connection`, `02311_system_zookeeper_insert`

All four fail identically: `Coordination::Exception: ZooKeeper root doesn't exist. You should create root
node .../ci/tmp/chroot/auxiliary_zookeeper2 before start.` when they `CREATE TABLE ... ENGINE =
ReplicatedMergeTree('zookeeper2:/...', ...)` or otherwise touch the `zookeeper2` (auxiliary) connection.

**Root cause, proven from source, not CAS-attributable:** `ci/jobs/functional_tests.py:507-515` sets
`has_stateful_tests = False` for a local full-suite run (the same gate documented for the `test.hits`/
`test.visits` gap in the run-1 triage). `ci/jobs/functional_tests.py:690-696` only calls
`CH.insert_system_zookeeper_config()` — which is the step, in
`ci/jobs/scripts/clickhouse_proc.py`'s `insert_system_zookeeper_config`, that inserts into
`system.zookeeper` to create the `auxiliary_zookeeper2` chroot root — when `has_stateful_tests` is true.
So on a local full-suite run this creation step is skipped for the same reason `prepare_stateful_data` is
skipped, and every test that depends on the `zookeeper2` connection having its root pre-created fails
with the same `KEEPER_EXCEPTION`. This is the same class of local-infra gap as class A, gated behind the
identical flag, not a CAS defect. `02735_system_zookeeper_auxiliary` was the brief's named item to check;
verdict: local config gap, confirmed, and the other three auxiliary-ZooKeeper tests share the exact same
cause.

### 2. `mount-lease-fence` — 2 tests: `01128_generate_random_nested`, `02581_share_big_sets_between_mutation_tasks` (NEW finding) {#2-mount-lease-fence-2-tests-01128-generate-random-nested-02581-share-big-sets-be}

Both fail with `NETWORK_ERROR` / `content-addressed disk 'cas_s3' -- mount lease not held` (Code 210),
not a plain PUT-timeout stderr line, even though both blocks also contain `WriteBufferFromS3` Timeout
lines from the same window (that's what puts them at risk of being folded into the PUT-timeout class by
a naive scan — checked explicitly and excluded).

**Root cause, from `ci/tmp/var/log/clickhouse-server/clickhouse-server.log`:** at `2026.09.04 04:08:37`
(server-local time), `CasMountLeaseRenewer` logs `CAS mount renewal 'stateless-ca-s3' fenced after 3
physical attempts in 9068 ms (classification=external_lease_deadline, confirmed_deadline_boot_ms=1400321960)`
— the lease renewer's three physical retry attempts for the CAS mount all failed inside the renewal
deadline (same S3-endpoint saturation window that produces the PUT-timeout class: this instant shows a
burst of `WriteBufferFromS3` timeouts on unrelated keys immediately before it). The fence classification
`ExternalLeaseDeadline` is implemented in
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:328` and is already
covered by unit tests (`gtest_cas_heartbeat.cpp:759`, `gtest_cas_pool.cpp:3710`,
`gtest_cas_event_log.cpp:291,451`) — the fence-under-deadline behavior itself is intentional, fail-close
design, not a bug. In the 371 log lines carrying `mount lease not held` in this window, the great
majority are `DatabaseCatalog: Cannot drop table ... Will retry later` (background drop retries that
self-heal) and `MutatePlainMergeTreeTask`/background-executor errors (background tasks that get
rescheduled). Only these two tests surfaced the transient condition to a **synchronous client-facing**
call instead of absorbing it internally:
  - `02581_share_big_sets_between_mutation_tasks`: an `ALTER TABLE` that waits synchronously for its
    mutation (`StorageMergeTree::waitForMutation`) returned the mutation's terminal Code 210 to the
    client as a query failure, rather than the harness/server retrying it transparently.
  - `01128_generate_random_nested`: the `INSERT`'s `CasRefLedger` ref-log append landed durable but the
    mount fence moved before the checkpoint frontier was published (`CasPool: CAS ref table ... NEEDS
    RECOVERY at committed-frontier publication fence`), and `executeQuery` surfaced that as
    `NETWORK_ERROR` to the client instead of retrying.

**This is a new finding, not previously tracked.** It is downstream of the same root contention already
tracked by `{#cas-s3-lane-put-timeout-logged-at-error}` and `{#ref-catalog-cas-starvation}` (one saturated
local S3-compatible endpoint under the full 16-way parallel suite), but it is a *different* symptom: a
genuine, correctly-classified TRANSIENT mount-lease loss reaching two synchronous client calls as a hard
query failure instead of being retried. Return item: whether `StorageMergeTree::waitForMutation` and the
`INSERT` path's ref-log append should retry once transparently on a `NETWORK_ERROR` classified as
CAS-transient (as background tasks already do) before surfacing to the client — owner: next engine task
on this branch, needs a design call (retry semantics for synchronous client-facing calls vs. background
tasks), not a mechanical fix.

### 3. `harness-hang-under-load` — 1 test: `00092_obfuscator` (the brief's named case) {#3-harness-hang-under-load-1-test-00092-obfuscator-the-brief-s-named-case}

The brief asked to classify the test the lldb hang-dump belonged to: it is `00092_obfuscator`
(`tests/queries/0_stateless/00092_obfuscator.sh`), timed out at the harness's 1200 s ceiling
(`Reason: Timeout! Killing process group`), followed by the harness's lldb-attach diagnostic dump, which
itself failed locally (`ModuleNotFoundError: No module named 'lldb.embedded_interpreter'` — a pre-existing
local-tooling gap, not new).

**Root cause of the hang:** `00092_obfuscator.sh`'s first line is `$CLICKHOUSE_CLIENT --query="SELECT
URL, Title, SearchPhrase FROM test.hits LIMIT 1000" > data.tsv`. `test.hits` is absent locally (class A —
same gate as above), so this redirect produces no rows; the subsequent `clickhouse-obfuscator <
data.tsv` was then observed (via the SIGSTOP thread dump) parked in `ParallelParsingInputFormat::read()`
on a condition variable rather than exiting on empty input, under the same heavily loaded host (16
parallel workers, the S3 endpoint saturated at that time) that produces the PUT-timeout and mount-lease
classes. This is the brief's own pre-given classification ("a client-tool timeout under load") confirmed
against the actual script and log; it is a consequence of the class-A gap plus host contention, not a CAS
defect, and it needs no separate fix.

## Appendix — full FAIL/BROKEN → class table {#appendix-full-fail-broken-class-table}

### A — stateful dataset absent (236): classic hits/visits tests (120) {#a-stateful-dataset-absent-236-classic-hits-visits-tests-120}

`00001_count_hits 00002_count_visits 00004_top_counters 00005_filtering 00006_agregates 00007_uniq
00008_uniq 00009_uniq_distributed 00010_quantiles_segfault 00011_sorting 00012_sorting_distributed
00013_sorting_of_nested 00014_filtering_arrays 00015_totals_and_no_aggregate_functions
00016_any_if_distributed_cond_always_false 00017_aggregation_uninitialized_memory
00020_distinct_order_by_distributed 00021_1_select_with_in 00021_2_select_with_in 00021_3_select_with_in
00022_merge_prewhere 00023_totals_limit 00030_array_enumerate_uniq 00031_array_enumerate_uniq
00032_aggregate_key64 00033_aggregate_key_string 00034_aggregate_key_fixed_string 00035_aggregate_keys128
00036_aggregate_hashed 00037_uniq_state_merge1 00039_primary_key 00040_aggregating_materialized_view
00041_aggregating_materialized_view 00042_any_left_join 00043_any_left_join 00044_any_left_join_string
00045_uniq_upto 00046_uniq_upto_distributed 00047_bar 00048_min_max 00049_max_string_if 00050_min_max
00051_min_max_array 00052_group_by_in 00053_replicate_segfault 00054_merge_tree_partitions
00055_index_and_not 00056_view 00059_merge_sorting_empty_array_joined 00060_move_to_prewhere_and_sets
00062_loyalty 00063_loyalty_joins 00065_loyalty_with_storage_join 00066_sorting_distributed_many_replicas
00067_union_all 00068_subquery_in_prewhere 00069_duplicate_aggregation_keys 00071_merge_tree_optimize_aio
00072_compare_date_and_string_index 00073_uniq_array 00074_full_join 00075_left_array_join
00078_group_by_arrays 00079_array_join_not_used_joined_column 00080_array_join_and_union
00081_group_by_without_key_and_totals 00082_quantiles 00083_array_filter 00084_external_aggregation
00085_monotonic_evaluation_segfault 00086_array_reduce 00087_where_0
00088_global_in_one_shard_and_rows_before_limit 00089_position_functions_with_non_constant_arg
00091_prewhere_two_conditions 00093_prewhere_array_join 00094_order_by_array_join_limit
00095_hyperscan_profiler 00097_constexpr_in_index 00139_like 00141_transform 00142_system_columns
00143_transform_non_const_default 00144_functions_of_aggregation_states
00145_aggregate_functions_statistics 00146_aggregate_function_uniq 00147_global_in_aggregate_function
00148_monotonic_functions_and_index 00149_quantiles_timing_distributed 00150_quantiles_timing_precision
00151_order_by_read_in_order 00152_insert_different_granularity 00153_aggregate_arena_race 00154_avro
00158_cache_dictionary_has 00160_decode_xml_component 00162_mmap_compression_none
00163_column_oriented_formats 00164_quantileBfloat16 00165_jit_aggregate_functions 00169_contingency
00172_early_constant_folding 00173_group_by_use_nulls 00174_distinct_in_order
00175_counting_resources_in_subqueries 00176_distinct_limit_by_limit_bug_43377 00178_quantile_ddsketch
00181_cross_join_compression 00182_simple_squashing_transform_bug 00183_prewhere_conditions_order
00184_parallel_hash_returns_same_res_as_hash 03582_pr_read_in_order_hits 03595_extract_url_parameters
03634_autopr_input_bytes_estimation 03634_autopr_output_bytes_estimation 03800_autopr_reuse_index_analysis
03801_autopr_input_bytes_estimation_query_with_subqueries 03830_no_zero_rows_with_unaligned_block_size
03927_autopr_input_bytes_estimation_prewhere_filter 04102_autopr_input_bytes_estimation_query_with_ctes`

Each fails with `UNKNOWN_TABLE`/`UNKNOWN_DATABASE` (Code 60/81) on `test.hits`/`test.visits`, or (for
`00142_system_columns`) a `system.columns` diff whose missing rows are exactly the `hits`/`visits`
columns — same root cause, different surfacing.

### A — tpcds (95, all `Database tpcds does not exist`) {#a-tpcds-95-all-database-tpcds-does-not-exist}

`04033_tpc_ds_q01..q99` except `q14`, `q24`, `q31` (already on the ignore-list — read from the S3 `web`
disk, a distinct cause) and `q35`, which **passed** (0.03 sec — it does not touch the `tpcds` database,
so it is unaffected by the local dataset gap).

### A — tpch (21, all `Database tpch does not exist`) {#a-tpch-21-all-database-tpch-does-not-exist}

`04040_tpc_h_q01 q02 q03 q04 q05 q06 q07 q08 q09 q10 q11 q12 q13 q14 q16 q17 q18 q19 q20 q21 q22`. `q15`
does not appear anywhere in `ci/tmp/test_result.txt` or the log at all (not run, not skipped-and-logged);
its file carries a `no-parallel` tag while this job ran with `--no-sequential` (parallel-only mode) —
consistent with it being pre-filtered out of the selected test set before execution, unrelated to CAS.

### PUT-timeout (42) — BACKLOG `{#cas-s3-lane-put-timeout-logged-at-error}`

`00800_low_cardinality_empty_array 00871_t64_codec_signed 00914_join_bgranvea 00953_moving_functions
01055_compact_parts_granularity 01514_empty_buffer_different_types 02233_set_enable_with_statement_cte_perf
02346_text_index_hint_map 02354_vector_search_with_huge_dimension 02366_asof_optimize_predicate_bug_37813
02435_rollback_cancelled_queries 02461_prewhere_row_level_policy_lightweight_delete
02479_race_condition_between_insert_and_droppin_mv 02516_projections_with_rollup
02521_lightweight_delete_and_ttl 02541_lightweight_delete_on_cluster 02785_text_with_whitespace_tab_field_delimiter
02943_variant_type_with_different_local_and_global_order 03015_optimize_final_rmt 03032_redundant_equals
03036_dynamic_read_shared_subcolumns_small 03214_backup_and_clear_old_temporary_directories
03254_trivial_merge_selector 03277_json_subcolumns_in_primary_key 03306_optimize_table_force_keyword
03312_sparse_column_tuple 03401_normal_projection_with_part_offset_no_sorting 03405_join_using_alias_constant
03459_numeric_indexed_vector_decode 03622_distributed_index_analysis_additional_table_filters
03709_tuple_inside_nullable_basic 03802_aggregating_simple_agg_func_tuple_element
03822_alias_column_skip_index_no_merge 03823_optimize_dry_run_rmt 04029_multiply_monotonicity
04041_vertical_merge_stop_start_race 04043_materialized_cte_serialize_query_plan
04053_merge_table_large_query_performance 04063_read_in_order_through_creating_sets
04141_text_index_alias_lambda_with_constants 04238_alter_rename_column_merge_race
04494_backup_metadata_round_trip`

Each has stdout that is otherwise correct; the sole failure trigger is one or more `<Error>
WriteBufferFromS3: S3Exception ... Timeout ... key cas_s3/cas/ref_catalog|.../_log/...` lines on stderr,
matching the BACKLOG entry's documented shape exactly.

### ignore-list (9) {#ignore-list-9}

`00163_shard_join_with_empty_table 01854_s2_cap_union 02224_s2_test_const_columns
02479_mysql_connect_to_self 02784_connection_string 03233_dynamic_in_functions 04033_tpc_ds_q14
04033_tpc_ds_q24 04033_tpc_ds_q31` — matches
`reference_ca_s3_lane_ignore_tests.md` exactly; no new evidence contradicting any entry.

### aux-zk-local-config-gap (4) {#aux-zk-local-config-gap-4}

`02311_system_zookeeper_insert 02442_auxiliary_zookeeper_endpoint_id 02735_system_zookeeper_auxiliary
02735_system_zookeeper_connection` — root-caused above.

### mount-lease-fence (2) {#mount-lease-fence-2}

`01128_generate_random_nested 02581_share_big_sets_between_mutation_tasks` — root-caused above, NEW
finding.

### catalog-starvation (1) {#catalog-starvation-1}

`01039_mergetree_exec_time` — BACKLOG `{#ref-catalog-cas-starvation}`.

### harness-hang-under-load (1) {#harness-hang-under-load-1}

`00092_obfuscator` — root-caused above.

## Concerns / follow-ups {#concerns-follow-ups}

1. **New finding to add to BACKLOG:** the mount-lease-fence class (2 tests) is not currently tracked
   anywhere. It is a correctly-classified, fail-close mount fence (unit-tested), but two client-facing
   synchronous paths (`StorageMergeTree::waitForMutation`, the `INSERT` ref-log append surfaced through
   `executeQuery`) do not retry through it the way background tasks do. Recommend filing this as its own
   BACKLOG item rather than folding it into the two existing PUT-timeout/starvation entries, since the
   fix surface (client-facing retry semantics) is different from either.
2. **aux-zk-local-config-gap is a real, if minor, gap in the local-run harness**, not CAS: `ci/jobs/functional_tests.py`'s
   `has_stateful_tests` gate also disables `insert_system_zookeeper_config`, so any local full-suite run
   will always fail these 4 auxiliary-ZooKeeper tests. Worth a one-line note next to the existing
   `has_stateful_tests` comment (not a CAS-branch concern; out of scope for this triage to fix).
3. **`ci/tmp/result_stateless_tests_amd_binary_cas_s3_storage_parallel.json` reports `status: ERROR`**
   with an "Invalid status [PENDING] for exit code [1]" message and an empty `results` array — this is a
   praktika local-wrapper artifact from running the job outside its normal CI harness expectations, not a
   test-content problem; `ci/tmp/test_result.txt` and the raw log are authoritative and were used
   throughout. Flagging in case a future automated consumer of the JSON result file gets confused by it.
4. No `Server died` / `Connection refused` entries and no server restart during the 8561 s run; the one
   process-group timeout (`00092_obfuscator`) is client-tool, not server-side.
