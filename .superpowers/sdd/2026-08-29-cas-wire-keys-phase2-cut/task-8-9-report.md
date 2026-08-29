# CAS wire-key phase 2 — Tasks 8 and 9 report

## Task 8 — `cas_ref_log`

Status: green; commit pending.

### Inventory

Before the cut, I searched both raw JSON (`rg -n '"we":'`) and escaped
JSON (`rg -nF '\"we\":'`) forms for every old spelling across
`src/Disks/tests/`, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`
comments/source, `tests/integration/`, `utils/ca-soak/`, and
`tests/queries/0_stateless/*cas*`. Overloaded hits were classified by codec:

| Old spelling | Ref-log worklist (files; hit count) | Non-ref-log hits left unchanged |
|---|---|---|
| `ns` | `CasRefLogFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_log_format.cpp` (1), `gtest_cas_ref_epoch_seal_format.cpp` (1), `gtest_cas_ref_ckpt.cpp` (1): 5 | ref-snapshot, ref-catalog, fold-seal, part-manifest, and the catalog stateless script |
| `we` | `CasRefLogFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_log_format.cpp` (1), `gtest_cas_ref_epoch_seal_format.cpp` (1), `gtest_cas_ref_ckpt.cpp` (1): 5 | ref-snapshot and JSON-writer |
| `rs` | `CasRefLogFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_log_format.cpp` (1), `gtest_cas_ref_epoch_seal_format.cpp` (5), `gtest_cas_ref_ckpt.cpp` (1): 9 | ref-snapshot |
| `!pse` | `CasRefLogFormat.cpp` (2: literal/comment), `gtest_cas_ref_epoch_seal_format.cpp` (8), `gtest_cas_orphan_manifest_sweep.cpp` (2), `gtest_cas_fsck.cpp` (2): 14 | none |
| `!pss` | `CasRefLogFormat.cpp` (2: literal/comment), `gtest_cas_ref_epoch_seal_format.cpp` (7): 9 | none |
| `rn` | `CasRefLogFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_log_format.cpp` (1): 3 | ref-snapshot |
| `ts` | `CasRefLogFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_log_format.cpp` (2): 4 | ref-snapshot and blob envelope |

No Task 8 ref-log parser was found in `tests/integration/`, `utils/ca-soak/`,
or `tests/queries/0_stateless/*cas*`; the only stateless `ns` hit is the
ref-catalog lookup in `05023_cas_dropns_leaked_namespace.sh` and was left
unchanged.

### Changed files

- `CasRefLogFormat.cpp`: meta keys now use `namespace`, `txn_epoch`,
  `txn_seq`, `!prev_epoch`, and `!prev_seq`; `set_published_at` uses `ref`
  and `published_ms`. Both-or-neither parsing and unknown-critical-key
  rejection are unchanged.
- `gtest_cas_encoding_pins.cpp`, `gtest_cas_ref_log_format.cpp`,
  `gtest_cas_ref_epoch_seal_format.cpp`, `gtest_cas_ref_ckpt.cpp`,
  `gtest_cas_orphan_manifest_sweep.cpp`, and `gtest_cas_fsck.cpp`: updated
  ref-log goldens and raw corruption splices.
- `Formats/README.md`: updated the ref-log registry row.

### Derived expectations and sentinels

`CommitRefChunkDurableBytesUnchangedByExtraction` moved from 201 bytes /
`6068c3d8bed1ecae98ec56902ef43d97` to 206 bytes /
`21c275ad44a6b47a4d6c389c0d71bb34`. These values came from the initial
full-gate failure after the exact canonical plaintext was updated; the same
test independently asserts that complete plaintext, including the new meta
line, before checking its compressed size and SipHash. The ref-log op-budget
helpers were not edited and pass through their real-writer tests.

Re-verified fired rejection sentinels:

- `CASRefCodec.DecodeRejectsRemovedPayloadFieldInOpRecord` rejects literal
  row key `pl` with a live `published_ms` field around it.
- `CASRefEpochSealFormat.DecodeRejectsUnknownCriticalKeyInMetaLine` rejects
  `!future_critical_field` with `UNKNOWN_FORMAT_VERSION`.

### Gate

Build: `ninja -C build unit_tests_dbms > build/build_wirekeys_p2_task8.log 2>&1`
completed successfully; the final rebuild log linked `src/unit_tests_dbms`
without diagnostics.

`grep -E '^\[  FAILED  \]|tests from .* ran|^\[  PASSED  \]' build/test_cas_p2_task8.log`:

```text
[==========] 2208 tests from 284 test suites ran. (163105 ms total)
[  PASSED  ] 2208 tests.
```

No matching `[  FAILED  ]` line was emitted. Deviations: none.

## Task 9 — `cas_ref_snapshot`

Status: green; commit pending.

### Inventory

Before the cut, I searched raw and escaped JSON forms of every old spelling
across `src/Disks/tests/`,
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`,
`tests/integration/`, `utils/ca-soak/`, and
`tests/queries/0_stateless/*cas*`. The target snapshot worklist was:

| Old spelling | Ref-snapshot worklist (files; hit count) | Non-ref-snapshot hits left unchanged |
|---|---|---|
| `ns` | `CasRefSnapshotFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_snapshot_format.cpp` (1): 3 | ref-catalog, fold-seal, part-manifest, catalog soak parser, and stateless catalog script |
| `we` | `CasRefSnapshotFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_snapshot_format.cpp` (1): 3 | JSON-writer |
| `rs` | `CasRefSnapshotFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_snapshot_format.cpp` (2): 4 | none |
| `lc` | `CasRefSnapshotFormat.cpp` (2: literal/comment), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_snapshot_format.cpp` (7): 10 | none |
| `k` | `CasRefSnapshotFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (2), `gtest_cas_ref_snapshot_format.cpp` (3): 6 | ref-catalog, fold-seal, GC outcomes, and soak row generator |
| `rn` | `CasRefSnapshotFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (2), `gtest_cas_ref_snapshot_format.cpp` (3): 6 | ref-log and a soak backlog comment |
| `ts` | `CasRefSnapshotFormat.cpp` (1), `gtest_cas_encoding_pins.cpp` (1), `gtest_cas_ref_snapshot_format.cpp` (3): 5 | blob envelope and soak telemetry |
| `pl` | `CasRefSnapshotFormat.cpp` (1) and `gtest_cas_ref_snapshot_format.cpp` (1): 2 rejected sentinel literals | ref-log sentinel |
| `rte` | `CasRefSnapshotFormat.cpp` (1) and `gtest_cas_ref_snapshot_format.cpp` (2): 3 rejected meta sentinel literals | fold-seal and GC hold grammar |
| `rts` | `CasRefSnapshotFormat.cpp` (1) and `gtest_cas_ref_snapshot_format.cpp` (2): 3 rejected meta sentinel literals | fold-seal and GC hold grammar |

No snapshot parser was found in `tests/integration/` or `utils/ca-soak/`.

### Changed files

- `CasRefSnapshotFormat.cpp`: meta keys are now `namespace`,
  `snapshot_epoch`, `snapshot_seq`, and `lifecycle`; rows use `kind`, `ref`,
  and committed-only `published_ms`. The sole `live` word constant is used by
  both writer and reader, with an explicit `RefLifecycle::Live` check.
- Snapshot row values are rendered by `refOwnerKindToWord` and parsed by
  `refOwnerKindFromWord`; local `c`/`p` tag constants were removed.
- `gtest_cas_encoding_pins.cpp` and `gtest_cas_ref_snapshot_format.cpp`:
  updated exact canonical JSON, the decimal-sequence pin, and all live-key
  sentinel fixtures.
- `Formats/README.md`: updated the ref-snapshot registry row. The stale
  ref-log key comment in `CasRefWireVocab.h` was also corrected to its Task 8
  spelling.

### Expectations and sentinels

The `RefSnapshotLive` literal canonical-text pin changed to the new semantic
meta keys, owner-kind words, and row keys. Task 9 has no size/hash derived pin
to update.

Re-verified fired rejection sentinels, each with new `lifecycle:"live"` meta
keys around the forbidden literal:

- `CASRefSnapshotCodec.DecodeRejectsRemovedPayloadFieldInCommittedRow` for
  row literal `pl`.
- `CASRefSnapshotCodec.DecodeRejectsRetiredRemoveTxnEpochField` for meta
  literal `rte`.
- `CASRefSnapshotCodec.DecodeRejectsRetiredRemoveTxnSequenceField` for meta
  literal `rts`.
- `CASRefSnapshotCodec.DecodeRejectsRetiredRemoveTxnFieldPair` for the pair.

### Gate

Build: `ninja -C build unit_tests_dbms > build/build_wirekeys_p2_task9.log 2>&1`
completed successfully; the log linked `src/unit_tests_dbms` without
diagnostics.

`grep -E '^\[  FAILED  \]|tests from .* ran|^\[  PASSED  \]' build/test_cas_p2_task9.log`:

```text
[==========] 2208 tests from 284 test suites ran. (163181 ms total)
[  PASSED  ] 2208 tests.
```

No matching `[  FAILED  ]` line was emitted. Deviations: none.
