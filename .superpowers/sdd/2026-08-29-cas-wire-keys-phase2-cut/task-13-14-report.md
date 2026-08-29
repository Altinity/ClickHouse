# CAS wire keys phase 2 — Tasks 13 and 14

## Task 13 — complete

### Inventory

The required three-form `hr` sweep was completed before implementation. It found no Task 13-owned
spelling: raw `"hr":` — 3 hits in `gtest_cas_gc_hold_grammar.cpp` (fold-seal hold grammar);
escaped `\"hr\":` — 0; bare `"hr"` — the same 3 fold-seal test hits. The requested root
`ContentAddressed/` path does not exist; the codec is under
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.

### Files

- `CasWireVocab.{h,cpp}`: added `TokenFields::build`, requiring `token_type` and `token` and
  decoding the type through the token-word table as `CORRUPTED_DATA`.
- `CasGcOutcomesFormat.cpp` and `CasRecordStreamFormat.cpp`: use that collector; the obsolete
  `have_tt`/`have_tv` requiredness locals are removed.
- `gtest_cas_gc_outcomes_format.cpp`, `gtest_cas_record_stream_format.cpp`, and
  `gtest_cas_wire_vocab.cpp`: updated and extended coverage.
- `Formats/README.md`: records that the two token fields are jointly required.

### Expectations and behavior

- `RecordTokenValueIsOptionalButTokenIdentityIsRequired` was renamed to
  `RecordRequiresCompleteBlobRefAndTokenGroups`.
- Kept its missing `algo` and `digest` negatives, now pinned to the collector-owned
  `CAS outcome log: blob ref missing ha/h`; kept its missing `token_type` negative and added the
  missing `token` negative, both pinned to `CAS outcome log: token missing token_type/token` with
  `CORRUPTED_DATA`.
- Replaced only the previous assertion that a missing `token` decoded as an empty value. The new
  message is trustworthy because `writeTokenFields` always writes both fields and the independent
  `TokenFieldsBuildsInAnyKeyOrderAndRequiresBothFields` test verifies collector construction.
- Updated the `cas_run` condemned-row message pins: missing `token_type` or `token` now reports
  `CAS cas_run: token missing token_type/token`; the other four physical fields report
  `CAS cas_run: condemned record missing pending/size/condemn_round/confirmed`. The test still
  independently proves all six encoded fields fail closed.

### Gate

Build: `ninja -C build unit_tests_dbms > build/build_wirekeys_p2_task13.log 2>&1` — green.

```text
[==========] 2215 tests from 284 test suites ran. (162850 ms total)
[  PASSED  ] 2215 tests.
```

`grep -aE '^\\[  FAILED  \\]|tests from .* ran|^\\[  PASSED  \\]' build/test_cas_p2_task13.log`
returned no `[  FAILED  ]` line; 2215 ran equals 2215 passed.

### Deviations

None.

## Task 14 — pending
