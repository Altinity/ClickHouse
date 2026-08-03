# Task 6 — remove the `ReaderExecutorModeledCostMsPerRequestedMiB` async metric

## Enumeration

`grep -n 'ReaderExecutorModeledCostMsPerRequestedMiB|prev_reader_executor'` tree-wide: **5 files**.
Three are code/test and were changed; two are `CHANGELOG.md` and its mirror
`docs/_includes/content/changelog.md`, released-history entries for PR #106968 that are never
retroactively edited and were left alone.

## Applied

- Deleted the whole "Experimental ReaderExecutor read-path efficiency KPI" block in
  `ServerAsynchronousMetrics::updateImpl` — the two counter reads, the `first_run` guard, the
  `ms_per_mib` computation, the `new_values[…]` publish, and the delta bookkeeping.
- Deleted the two members `prev_reader_executor_cost_us` / `prev_reader_executor_requested_bytes`
  from `ServerAsynchronousMetrics.h`.
- Deleted the now-dangling `namespace ProfileEvents { extern const Event … }` forward-declaration
  block at the top of `ServerAsynchronousMetrics.cpp` — after the removal the file has zero
  `ReaderExecutor` references.
- `first_run` is still consumed by `updateHeavyMetricsIfNeeded`, so removing this block leaves no
  unused parameter.
- The two ProfileEvents `ReaderExecutorModeledCostMicroseconds` / `ReaderExecutorRequestedBytes` and
  their descriptions in `ProfileEvents.cpp` are untouched.

## Test

`04328_reader_executor_kpi_async_metric.{sql,reference}` →
`04328_reader_executor_modeled_cost_profile_event.{sql,reference}` (`git mv`).

Kept: the tag line, the table setup, the `use_reader_executor` probe query, and assertion (1) —
`ProfileEvents['ReaderExecutorModeledCostMicroseconds'] > 0` from `query_log`.
Dropped: both `SYSTEM RELOAD ASYNCHRONOUS METRICS` ticks, the `asynchronous_metric_log` flush and
assertion (2), and the header prose describing the async metric. The reference is now a single `1`
line (it was two). The probe's `log_comment` was renamed with the file.

The surviving assertion is not vacuous: it reads the ProfileEvent out of `query_log` for that one
probe query, so it fails if the executor stops recording a modeled cost. It no longer proves anything
about asynchronous metrics — by design, since the metric it checked no longer exists.

## Verification

- `grep -n '04328_reader_executor_kpi|ReaderExecutorModeledCostMsPerRequestedMiB'` tree-wide: only
  the two changelog files.
- `ninja -C build clickhouse unit_tests_dbms` → `NINJA_EXIT=0` (`build/obscure_t6_build.log`).
  The test itself is a stateless test and, per this effort's verification policy, was not executed.
