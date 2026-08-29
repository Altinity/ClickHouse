# Phase 2 tasks 6–7 report

## Task 6 — server-root singletons and lease member rename

Status: complete.

Inventory before the flip, searched in `src/Disks/tests/`, `ContentAddressed/` comments and
sources, `tests/integration/`, and `utils/ca-soak/` using both raw and escaped forms:

| Old spelling | Raw matches (files) | Escaped matches (files) |
|---|---:|---:|
| `su` | 1 (`gtest_cas_server_root_format.cpp`) | 2 (`gtest_cas_server_root_format.cpp`, `gtest_cas_json_writer.cpp`) |
| `rt` | 0 | 1 (`gtest_cas_server_root_format.cpp`) |
| `nwe` | 0 | 1 (`gtest_cas_server_root_format.cpp`) |
| `we` | 2 (`gtest_cas_server_root_format.cpp`, `gtest_cas_ref_ckpt.cpp`) | 6 (`gtest_cas_server_root_format.cpp`, `gtest_cas_ref_ckpt.cpp`, `gtest_cas_encoding_pins.cpp`, `gtest_cas_json_writer.cpp`, `gtest_cas_ref_epoch_seal_format.cpp`, `gtest_cas_ref_log_format.cpp`, `gtest_cas_ref_snapshot_format.cpp`) |
| `hn` | 0 | 2 (`gtest_cas_server_root_format.cpp`, `gtest_cas_json_writer.cpp`) |
| `sat` | 0 | 1 (`gtest_cas_server_root_format.cpp`) |
| `eat` | 0 | 2 (`gtest_cas_server_root_format.cpp`, `gtest_cas_json_writer.cpp`) |
| `ma` | 0 | 1 (`gtest_cas_server_root_format.cpp`) |
| `fen` | 0 | 2 (`gtest_cas_server_root_format.cpp`, `gtest_cas_json_writer.cpp`) |

Only `gtest_cas_server_root_format.cpp` owns these server-root spellings. The other hits are
unrelated formats or generic JSON-writer coverage, so they were intentionally unchanged. No
integration or soak parser/assertion read any task-6 key.

Changed files: `CasServerRootFormats.cpp`, `CasServerRootFormats.h`, `Formats/README.md`,
`StorageSystemContentAddressedMounts.cpp`, every `src/Disks/` lease-member use site, and their
tests. `MountLease::min_active` became `min_active_build_sequence` at every use site, including
the heartbeat callback/argument names, GC eligibility, decommissioning, inspection output, the
system-table projection, helpers, and test expectations. The codec retains the documented
`UINT64_MAX` clean-farewell sentinel.

Updated literal expectations: `su` → `server_uuid`, `rt` → `retired_at_ms`, `nwe` →
`next_writer_epoch`, `we` → `writer_epoch`, `hn` → `hostname`, `sat` → `started_at_ms`, `eat` →
`expires_at_ms`, `ma` → `min_active_build_sequence`, and `fen` → `gc_fenced`. `pid`, `seq`, and
`write_attempt_id` remain unchanged. There were no derived size/hash pins for this format.

Build: `ninja -C build unit_tests_dbms` succeeded after correcting the system-table projection
that compiled against the renamed member.

Gate lines (the log contains a binary diagnostic byte, so `grep -aE` was required to read the
same requested patterns):

```text
[==========] 2207 tests from 284 test suites ran. (163144 ms total)
[  PASSED  ] 2207 tests.
```

There were no `[  FAILED  ]` lines; 2207 run equals 2207 passed.

Deviations: none.

## Task 7 — `cas_ref_ckpt` and strict non-alias

Status: complete.

Inventory before the flip, searched in `src/Disks/tests/`, `ContentAddressed/` comments and
sources, `tests/integration/`, and `utils/ca-soak/` using both raw and escaped forms:

| Old spelling | Raw matches (files) | Escaped matches (files) |
|---|---:|---:|
| `le` | 44 (4: `gtest_cas_ref_ckpt.cpp`, `gtest_cas_recovery_grounding.cpp`, plus unrelated Prometheus integration generators) | 1 (1: `gtest_cas_ref_ckpt.cpp`) |
| `cte` | 6 (2: `gtest_cas_ref_ckpt.cpp`, `gtest_cas_recovery_grounding.cpp`) | 1 (1: `gtest_cas_ref_ckpt.cpp`) |
| `cts` | 3 (2: `gtest_cas_ref_ckpt.cpp`, `gtest_cas_recovery_grounding.cpp`) | 1 (1: `gtest_cas_ref_ckpt.cpp`) |
| `cse` | 3 (1: `gtest_cas_ref_ckpt.cpp`) | 1 (1: `gtest_cas_ref_ckpt.cpp`) |
| `css` | 2 (1: `gtest_cas_ref_ckpt.cpp`) | 1 (1: `gtest_cas_ref_ckpt.cpp`) |
| `lse` | 1 (1: `gtest_cas_ref_ckpt.cpp`) | 1 (1: `gtest_cas_ref_ckpt.cpp`) |
| `lss` | 2 (1: `gtest_cas_ref_ckpt.cpp`) | 1 (1: `gtest_cas_ref_ckpt.cpp`) |

The Prometheus `le` hits are unrelated identifiers. No `tests/integration/` or `utils/ca-soak/`
parser or assertion reads `cas_ref_ckpt`; therefore none required an update. The recovery-grounding
raw-body mutation needles were updated at its `life_epoch`, `committed_seq`, and `committed_epoch`
mutations.

Changed files: `CasRefCkptFormat.cpp`, `Formats/README.md`, `gtest_cas_ref_ckpt.cpp`,
`gtest_cas_recovery_grounding.cpp`, and `gtest_cas_ref_ckpt_join.cpp`.

Updated literal expectations: `le` → `life_epoch`, `cte` → `committed_epoch`, `cts` →
`committed_seq`, `cse` → `snapshot_epoch`, `css` → `snapshot_seq`, `lse` → `seal_epoch`, and
`lss` → `seal_seq`. `RejectsOldCommittedEpochKeyRatherThanAliasingIt` deliberately carries the
old `"cte":"9"` spelling and asserts the strict reader's `CORRUPTED_DATA` code.

Derived pins: `CKPT_WORST_CASE_ENCODED_BYTES` changed from 234 to 296. The first required full
gate printed `worst_bytes = 296`, so that literal comes from the failing run rather than an
estimate. `RoundTripsEveryFieldCombination` independently proves the checkpoint content's
semantic decode/encode behavior. The unrelated ref-log pin pair in
`CommitRefChunkDurableBytesUnchangedByExtraction` remains `201u` and
`6068c3d8bed1ecae98ec56902ef43d97`.

Build: `ninja -C build unit_tests_dbms` succeeded after the size-pin update.

Gate lines (the log contains a binary diagnostic byte, so `grep -aE` was required to read the
same requested patterns):

```text
[==========] 2208 tests from 284 test suites ran. (162930 ms total)
[  PASSED  ] 2208 tests.
```

There were no `[  FAILED  ]` lines; 2208 run equals 2208 passed.

Deviations: none.
