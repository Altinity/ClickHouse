# Step 9 Slice C — delete durable terminal-snapshot DTO residue

## Scope

This is the mechanical half of Task 5 Step 9. The marker/publication work is already gone. Remove only the durable `RefTableSnapshot` lifecycle/terminal-id surface while preserving the in-memory terminal state machine exactly.

## Required behavior

1. `RefTableState::Removed` and its exact `remove_txn_id` remain unchanged. Terminal log replay, removal ordering, mutation refusal, stale-handle behavior, and fold cleanup evidence remain unchanged.
2. `RefTableSnapshot` no longer has `lifecycle` or `remove_txn_id` members. It represents only a live generation-8 snapshot.
3. The generation-8 canonical wire remains byte-identical and still contains mandatory `"lc":"live"`. Do not remove or reorder this field and do not bump `G_BUILD`.
4. Encoding writes literal `lc = live`; decoding requires the `lc` field and accepts exactly `live`.
5. Decoder explicitly rejects retired `rte` or `rts`: reject `rte` alone, `rts` alone, and both together. They must not fall through tolerant unknown-field skipping.
6. `stateFromSnapshot` explicitly initializes the returned in-memory state as `Live`. The default `RefTableState` is `Removed`; omission is a correctness defect.
7. `snapshotOf` rejects every non-`Live` state with `CORRUPTED_DATA` before constructing a DTO.
8. Narrow `snapshotFramingSize` to `(ns, snapshot_id, row_count)` while preserving its exact byte count.
9. `CasInspect` stops rendering snapshot `lifecycle` and snapshot `remove_txn_id`. Do not remove fold-seal cleanup evidence `remove_txn_id`; it is different authoritative state.
10. Clean stale `CasRefWireVocab` and protocol contracts. Shared `RefTxnId` writer/validator helpers remain.

## TDD and tests

- Start with raw generation-8 wire tests in `gtest_cas_ref_snapshot_format.cpp`: mandatory `lc`, reject `lc:"removed"`, reject `rte` only, reject `rts` only, reject both. Run them before production edits. If current code already rejects a case, record it as GREEN characterization and prove non-vacuity with a temporary decoder mutation that makes the exact test fail, then restore.
- Replace DTO-based terminal encoder matrices; the invalid terminal shape must be unconstructible through the DTO.
- In `gtest_cas_ref_statemachine.cpp`, preserve proof that terminal replay remains `Removed` with the exact removal id, and make `snapshotOf` on that state throw.
- Update focused inspect assertions so snapshot JSON has neither retired field.
- Update compile fallout mechanically in fixtures/benchmarks. Avoid `gtest_cas_encoding_pins.cpp` unless field removal makes it fail to compile; if necessary, remove only retired DTO assignments/assertions and do not modify unrelated generation-8 goldens.
- Mutation controls must include: accepting retired wire fields, forgetting the explicit `Live` initialization in `stateFromSnapshot`, and allowing `snapshotOf(Removed)`.

## Files and coordination

Expected owned production files: `Formats/CasRefSnapshotFormat.{h,cpp}`, `Pool/CasRefProtocol.{h,cpp}`, `Tools/CasInspect.cpp`, comment-only `Formats/CasRefWireVocab.h`.

Expected tests: `gtest_cas_ref_snapshot_format.cpp`, `gtest_cas_ref_statemachine.cpp`, focused inspect test if present, and minimal compile fallout.

Do not edit or commit `.superpowers/sdd/task-5-report.md`. Write the implementation report to `.superpowers/sdd/task-5-step9-c-report.md`. Do not switch branches, rebase, amend, or overwrite shared dirty work. Commit only owned files plus this brief/report after independent review approval.

## Verification and report

Use the existing `build_debug` tree. Redirect every build/test to unique `build_debug/step9_c_*` logs and have a separate reviewer analyze each build/test log. Report BASE, exact commits, file list, commands, results, mutation failures/restoration checks, and any shared-tree blockers.
