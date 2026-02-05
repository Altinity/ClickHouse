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
