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

## Task 14 — complete

### Inventory

The required three-form `hr` sweep covered `src/Disks/tests/`, the actual codec root
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, `tests/integration/`,
`utils/ca-soak/`, and `tests/queries/0_stateless/*cas*`.

- Raw `"hr":`: 3 hits in `gtest_cas_gc_hold_grammar.cpp`, all fold-seal hold-grammar literals.
- Escaped `\"hr\":`: 1 hit in `gtest_cas_fold_seal_format.cpp`, the fold-seal ref-life golden.
- Bare `"hr"`: 3 hits in `gtest_cas_gc_hold_grammar.cpp`, the same fold-seal hold-grammar
  literals; no Python soak-card key manipulation was present.

The wider old-key worklist resolved to `CasFoldSealFormat.cpp`, its format/hold-grammar tests, and
the real-encoder reservation helpers in `CasRefCatalogFormat.cpp` and its tests. `rte`/`rts` hits in
the ref-snapshot codec and tests are its rejected sentinels, not fold-seal fields, and were retained.
Unrelated `k` and `g` hits in catalog, backend, integration, and generic JSON tests were retained.

### Files

- `CasFoldSealFormat.{h,cpp}`: changed metadata, record, row, and summary keys to their semantic
  names; changed record tags to `ref_life`, `blob_run`, and `condemned`; left `cls` unchanged.
- `gtest_cas_fold_seal_format.cpp` and `gtest_cas_gc_hold_grammar.cpp`: updated all literal fold-seal
  goldens and mutation fields, including every ref-life variant, blob-run, and condemned summary.
- `CasRefCatalogFormat.cpp` and `gtest_cas_ref_catalog.cpp`: updated comments naming the record tag;
  their real-encoder reservation tests need no manually maintained byte values.
- `Formats/README.md`: documents the new fold-seal metadata keys and tags.

### Expectations and derived pins

All changed goldens remain literal. The battery golden now begins with
`generation`/`parent_generation`, and its rows use `kind` with `ref_life`, `blob_run`, and
`condemned`; the full ref-life golden also spells every renamed hold and cleanup-evidence key.
There were no size or SipHash numeric pins to re-derive. The semantic assertion that makes the
changed encoder bytes trustworthy is
`CASRefCatalogAdmission.ReservationCoversActualWidestLegalRowsAcrossDecimalTransitions`; it passed
with the helpers measuring through `encodeFoldSeal`. `Predicate2AcceptsEqualityRefusesOneEntryOver`
also passed, confirming the derived admission boundary.

### Gate

Build: `ninja -C build unit_tests_dbms > build/build_wirekeys_p2_task14.log 2>&1` — green.

```text
[==========] 2215 tests from 284 test suites ran. (162716 ms total)
[  PASSED  ] 2215 tests.
```

`grep -aE '^\\[  FAILED  \\]|tests from .* ran|^\\[  PASSED  \\]' build/test_cas_p2_task14.log`
returned no `[  FAILED  ]` line; 2215 ran equals 2215 passed.

### Deviations

None.
