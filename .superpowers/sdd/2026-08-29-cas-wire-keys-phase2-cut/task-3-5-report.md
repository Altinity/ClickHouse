# Phase 2 singleton-cut report

## Task 3 — `cas_blob_meta`

- Status: complete.
- Inventory before the flip (scoped to blob-meta fixtures and codec comments): raw `st`: `tests/integration/test_cas_gcs/gcs_mocks/server.py` (1) and `tests/integration/test_cas_gcs/test.py` (2); escaped `\"st\":`: `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (3). Raw `cr`: `tests/integration/test_cas_gcs/gcs_mocks/server.py` (1); escaped `\"cr\":`: `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (4). Raw `sz`: none in the blob-meta scope; escaped `\"sz\":`: `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (4). Codec worklist: `CasBlobMetaFormat.cpp` (three `BlobMetaWire` values and the missing-key diagnostic). Other `sz` occurrences belonged to run/manifest or envelope vocabularies and were not changed.
- Files: `CasBlobMetaFormat.cpp`, `gtest_cas_blob_meta_format.cpp`, `gcs_mocks/server.py`, `test.py`, `Formats/README.md`.
- Updated expectations: `st` → `state`; `cr` → `condemn_round`; `sz` → `size`. State words remain `clean` and `condemned`.
- Gate: `[==========] 2205 tests from 284 test suites ran. (162701 ms total)`; no `[  FAILED  ]` line (zero failures).
- Deviations: none. The named integration assertions were updated but are outside the unit gate.
