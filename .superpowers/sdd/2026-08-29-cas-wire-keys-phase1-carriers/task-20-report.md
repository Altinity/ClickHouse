# Task 20 report

Status: complete.

## Files

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp`
- `src/Disks/tests/cas_format_test_battery.h`
- `src/Disks/tests/gtest_cas_text_format.cpp`
- `src/Disks/tests/gtest_cas_blob_envelope_format.cpp`
- `src/Disks/tests/gtest_cas_blob_meta_format.cpp`
- `src/Disks/tests/gtest_cas_format_battery.cpp`
- `src/Disks/tests/gtest_cas_ref_log_format.cpp`
- `src/Disks/tests/gtest_cas_ref_epoch_seal_format.cpp`
- `src/Disks/tests/gtest_cas_ref_snapshot_format.cpp`
- `src/Disks/tests/gtest_cas_ref_catalog.cpp`
- `src/Disks/tests/gtest_cas_part_manifest_format.cpp`
- `src/Disks/tests/gtest_cas_fold_seal_format.cpp`
- `src/Disks/tests/gtest_cas_gc_state_format.cpp`
- `src/Disks/tests/gtest_cas_gc_outcomes_format.cpp`
- `src/Disks/tests/gtest_cas_server_root_format.cpp`
- `src/Disks/tests/gtest_cas_ref_ckpt.cpp`
- `src/Disks/tests/gtest_cas_gc_maintenance_state_format.cpp`
- `src/Disks/tests/gtest_cas_record_stream_format.cpp`

## Battery closure

`allRegisteredFormatIds` projects the anonymous-namespace `TRAITS` array; no second list of registered formats was added. Static `CAS_BATTERY_COVERS` registrars cover `Blob`, `BlobMeta`, `PoolMeta`, `RefLog` (both its ordinary and epoch-seal call sites), `RefSnapshot`, `RefCatalog`, `PartManifest`, `FoldSeal`, `GcState`, `GcHeartbeat`, `GcOutcomes`, `Owner`, `ServerEpoch`, `MountLease`, `RefCkpt`, `GcMaintenanceState`, and `RunFile`.

The new `RefCkpt` golden is `currentFormatHeader("cas_ref_ckpt") + "{\"le\":\"7\",\"cte\":\"9\",\"cts\":\"11\",\"cse\":\"9\",\"css\":\"10\",\"lse\":\"8\",\"lss\":\"12\"}\n"`; it is the literal record already pinned by `CommittedThroughHasCanonicalExactWireEncoding`. The new `GcMaintenanceState` golden is `currentFormatHeader("cas_gc_maintenance_state") + "{\"cur\":\"cas/ns/a\"}\n"`, derived from `GcMaintenanceState{.janitor_cursor = "cas/ns/a"}`. The new `RunFile` golden is the current `cas_run` source-edge header, the literal source-edge line `{"b":"0100000000000000000000000000000002","s":"00000000000000000000000000000005","m":"edge"}`, and `{"n":1}`; the record line was copied from `CASEncodingPins.SourceEdgeRunLines`.

## Carried tests

These changes are included in the same commit as the battery closure. The outcomes splice test proves a missing `tv` decodes as an empty token value, while each missing `ha`, `h`, or `tt` reports `CORRUPTED_DATA` with `CAS outcome log: record missing ha/h/tt`.

The ref-log splice test proves both old and new binding groups have the current symmetric behavior: no keys leaves the optional binding absent, while a partially present group reports `CORRUPTED_DATA`.

## Gate

`ninja -C build unit_tests_dbms > build/build_wirekeys_task20.log 2>&1` passed: 71 steps, `build/src/unit_tests_dbms` linked.

`build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas_task20.log 2>&1` passed: 2212 tests, 0 failures, 2 disabled tests, 162789 ms.

## Deviations

`clang-format` is not installed in the checkout. No persisted wire bytes changed.
