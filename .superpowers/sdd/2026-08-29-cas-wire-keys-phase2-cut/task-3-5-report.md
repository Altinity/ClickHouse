# Phase 2 singleton-cut report

## Task 3 — `cas_blob_meta`

- Status: complete.
- Inventory before the flip (scoped to blob-meta fixtures and codec comments): raw `st`: `tests/integration/test_cas_gcs/gcs_mocks/server.py` (1) and `tests/integration/test_cas_gcs/test.py` (2); escaped `\"st\":`: `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (3). Raw `cr`: `tests/integration/test_cas_gcs/gcs_mocks/server.py` (1); escaped `\"cr\":`: `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (4). Raw `sz`: none in the blob-meta scope; escaped `\"sz\":`: `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (4). Codec worklist: `CasBlobMetaFormat.cpp` (three `BlobMetaWire` values and the missing-key diagnostic). Other `sz` occurrences belonged to run/manifest or envelope vocabularies and were not changed.
- Files: `CasBlobMetaFormat.cpp`, `gtest_cas_blob_meta_format.cpp`, `gcs_mocks/server.py`, `test.py`, `Formats/README.md`.
- Updated expectations: `st` → `state`; `cr` → `condemn_round`; `sz` → `size`. State words remain `clean` and `condemned`.
- Gate: `[==========] 2205 tests from 284 test suites ran. (162701 ms total)`; no `[  FAILED  ]` line (zero failures).
- Deviations: none. The named integration assertions were updated but are outside the unit gate.

## Task 4 — `cas_pool_meta` and `algos_used`

- Status: complete.
- Inventory before the flip (scoped to pool-meta fixtures and codec comments): raw `pid`, `hln`, `gcs`, `mrg`, and `alg`: `CasPoolMetaFormat.h` (one canonical body example containing each). Escaped `\"pid\":`, `\"hln\":`, `\"gcs\":`, `\"mrg\":`, and `\"alg\":`: `src/Disks/tests/gtest_cas_format_battery.cpp` (one each). Other raw `pid` and `gcs` hits belonged to server-root and GC-state formats and were not changed.
- Files: `CasPoolMetaFormat.{h,cpp}`, `CasTextFormat.{h,cpp}`, `gtest_cas_format_battery.cpp`, `gtest_cas_text_format.cpp`, `gtest_cas_pool.cpp`, `Formats/README.md`.
- Updated expectations: `pid` → `pool_id`; `hln` → `blob_header_len`; `gcs` → `gc_shards`; `mrg` → `min_reader_generation`; `alg` string → `algos_used` JSON word array. The direct byte-shape assertion is `{"algos_used":["ch128","sha256"]}`. The six negative pool fixtures (old joined string, non-string element, empty array, unsorted array, duplicate array, and unknown word) each require `CORRUPTED_DATA`; direct reader non-array and non-string-element negatives do too.
- Gate: `[==========] 2207 tests from 284 test suites ran. (162963 ms total)`; no `[  FAILED  ]` line (zero failures).
- Deviations: none. The initial gate exposed only two test setup/assertion mistakes caused by the new encoder validation and an unclosed test object; the final gate above is green.

## Task 5 — GC state, heartbeat, and maintenance state

- Status: complete.
- Inventory before the flip (scoped to the owning GC fixtures and codec comments): escaped `rnd` (3), `gcs` (2), `sg` (3), `spt` (3), `sa` (3), `msc` (3), `lo` (3), `ls` (3), `by` (2), and heartbeat `seq` (2) in `gtest_cas_gc_state_format.cpp`; escaped `cur` (9) in `gtest_cas_gc_maintenance_state_format.cpp`. Raw hits outside those files belonged to the blob-envelope, pool-meta, or server-root formats and were not changed. Codec worklist: both local wire-constant blocks and their missing-key comments/diagnostics.
- Files: `CasGcStateFormat.cpp`, `CasGcMaintenanceStateFormat.cpp`, `gtest_cas_gc_state_format.cpp`, `gtest_cas_gc_maintenance_state_format.cpp`, `Formats/README.md`.
- Updated expectations: GC state `rnd`/`gcs`/`sg`/`spt`/`sa`/`msc`/`lo`/`ls` → `round`/`gc_shards`/`snap_generation`/`snap_pruned_through`/`snap_attempt`/`manifest_sweep_cursor`/`lease_owner`/`lease_seq`; heartbeat `by`/`seq` → `owner`/`hb_seq`; maintenance `cur` → `janitor_cursor`.
- Gate: `[==========] 2207 tests from 284 test suites ran. (162898 ms total)`; no `[  FAILED  ]` line (zero failures).
- Deviations: none.
