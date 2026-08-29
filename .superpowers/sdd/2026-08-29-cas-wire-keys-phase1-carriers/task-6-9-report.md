# CAS wire-keys phase 1 carriers report {#cas-wire-keys-phase1-carriers-report}

## Task 7 {#task-7}

Status: complete.

Files changed: `CasPoolMetaFormat.cpp`, `gtest_cas_format_battery.cpp`.

Message-text decision: reader error messages are unchanged; only the compared key literals became named `WireKey` constants.

Gate results: `ninja -C build unit_tests_dbms` succeeded; `CAS*` passed 2199 tests with 0 failures.

Deviation: the prescribed test assertion referenced `DB::Cas::tests::expectThrowsCode`, which is not available through this test's lightweight includes. The equivalent anonymous-namespace `expectThrowsCode` pattern was added locally, avoiding the heavy test-helper dependency.

## Task 8 {#task-8}

Status: complete.

Files changed: `CasGcStateFormat.cpp`, `CasGcMaintenanceStateFormat.cpp`.

Message-text decision: reader error messages and strictness are unchanged; only the compared key literals became named `WireKey` constants.

Gate results: `ninja -C build unit_tests_dbms` succeeded; `CAS*` passed 2199 tests with 0 failures.

Deviation: none.

## Task 9 {#task-9}

Status: complete.

Files changed: `CasServerRootFormats.cpp`.

Message-text decision: reader error messages and required-field checks are unchanged; only the compared key literals became named `WireKey` constants.

Gate results: `ninja -C build unit_tests_dbms` succeeded; `CAS*` passed 2199 tests with 0 failures.

Deviation: none.
