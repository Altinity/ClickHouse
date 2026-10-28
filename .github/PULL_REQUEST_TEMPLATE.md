<!---
A technical comment, you are free to remove or leave it as it is when PR is created
The following categories are used in the next scripts, update them accordingly
utils/changelog/changelog.py
tests/ci/cancel_and_rerun_workflow_lambda/app.py
-->
### Changelog category (leave one):
- New Feature
- Experimental Feature
- Improvement
- Performance Improvement
- Backward Incompatible Change
- Build/Testing/Packaging Improvement
- Documentation (changelog entry is not required)
- Critical Bug Fix (crash, data loss, RBAC) or LOGICAL_ERROR
- Bug Fix (user-visible misbehavior in an official stable release)
- CI Fix or Improvement (changelog entry is not required)
- Not for changelog (changelog entry is not required)


### Changelog entry (a [user-readable short description](https://github.com/ClickHouse/ClickHouse/blob/master/docs/changelog_entry_guidelines.md) of the changes that goes into CHANGELOG.md):
...

### Documentation entry for user-facing changes
...

#### Exclude tests:
- [ ] <!---ci_exclude_fast--> Fast test
- [ ] <!---ci_exclude_integration--> Integration Tests
- [ ] <!---ci_exclude_stateless--> Stateless tests
- [ ] <!---ci_exclude_stateful--> Stateful tests
- [ ] <!---ci_exclude_performance--> Performance tests
- [ ] <!---ci_exclude_asan--> All with ASAN
- [ ] <!---ci_exclude_tsan--> All with TSAN
- [ ] <!---ci_exclude_msan--> All with MSAN
- [ ] <!---ci_exclude_ubsan--> All with UBSAN
- [ ] <!---ci_exclude_coverage--> All with Coverage
- [ ] <!---ci_exclude_aarch64--> All with Aarch64
- [ ] <!---ci_exclude_regression--> All Regression
- [ ] <!---no_ci_cache--> Disable CI Cache

#### Regression jobs to run:
- [ ] <!---ci_regression_common--> Fast suites (mostly <1h)
- [ ] <!---ci_regression_aggregate_functions--> Aggregate Functions (2h)
- [ ] <!---ci_regression_alter--> Alter (1.5h)
- [ ] <!---ci_regression_benchmark--> Benchmark (30m)
- [ ] <!---ci_regression_clickhouse_keeper--> ClickHouse Keeper (1h)
- [ ] <!---ci_regression_iceberg--> Iceberg (2h)
- [ ] <!---ci_regression_ldap--> LDAP (1h)
- [ ] <!---ci_regression_oauth--> OAuth (5m)
- [ ] <!---ci_regression_parquet--> Parquet (1.5h)
- [ ] <!---ci_regression_rbac--> RBAC (1.5h)
- [ ] <!---ci_regression_ssl_server--> SSL Server (1h)
- [ ] <!---ci_regression_s3--> S3 (2h)
- [ ] <!---ci_regression_s3_export--> S3 Export (2h)
- [ ] <!---ci_regression_swarms--> Swarms (30m)
- [ ] <!---ci_regression_tiered_storage--> Tiered Storage (2h)
