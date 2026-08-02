# Step 9 Slice C implementation report

## Scope and history

- BASE: `c13597c9d8348f87c408293c0fa394518cedfbf6` (`Test CAS namespace removal catalog cut`).
- The shared worktree advanced while Slice C was under test. Commit
  `224aacd8eb96f6da6e0b2457dad1b09ac563d3d5` accidentally absorbed the DTO, codec,
  protocol-contract, fixture, and test hunks together with another agent's namespace-removal work.
  No history surgery, rebase, or amend was attempted.
- Commit `6c0a5453bf72350e56022f3b6bf2dad4bdd21d5f` added the independently required
  `snapshotOf` terminal-state refusal that was not present in `224aacd8eb9`.
- Audit after those commits found one overwritten Slice C requirement: `CasInspect` still rendered
  a synthetic snapshot lifecycle. The follow-up commit containing this report removes that field and
  restores the focused negative assertion.
- The protected aggregate report `.superpowers/sdd/task-5-report.md` was neither edited nor staged
  by this slice.

## Final requirement audit

1. The in-memory terminal state machine remains intact. `RefTableState` still defaults to
   `RefLifecycle::Removed`, still owns optional `remove_txn_id`, and terminal replay still assigns
   the exact transaction id. Removal ordering, terminal log replay, mutation refusal, and fold-seal
   cleanup evidence were not removed.
2. `RefTableSnapshot` contains only `ns`, `snapshot_id`, committed rows, and precommits. It has
   no lifecycle or removal-id member.
3. The generation-8 wire remains byte-identical. The canonical golden still starts with
   `{"type":"cas_ref_snap","v":8}` and its metadata still contains the same ordered
   `"lc":"live"` field. No generation bump was made.
4. `writeSnapshotMeta` writes literal `live`. `decodeRefTableSnapshot` requires `lc` and
   rejects every word except `live`.
5. The decoder has an explicit `rte`/`rts` rejection branch. Raw tests cover `rte` alone,
   `rts` alone, and both together, so tolerant unknown-field skipping cannot accept the retired
   surface.
6. `stateFromSnapshot` explicitly assigns `RefLifecycle::Live`; it does not rely on the
   opposite-valued `RefTableState` default.
7. `snapshotOf` rejects every non-live state with `CORRUPTED_DATA` before constructing its DTO.
   The test first proves terminal replay retained `Removed` plus the exact removal id.
8. `snapshotFramingSize` is narrowed to `(ns, snapshot_id, row_count)` and is property-tested
   against the full encoder.
9. Snapshot JSON from `CasInspect` contains neither lifecycle nor removal id. Fold-seal
   `cleanup_evidence.remove_txn_id` remains rendered and was explicitly checked during the audit.
10. Stale snapshot-terminal wording is gone from `CasRefWireVocab` and the protocol contracts.
    Shared `RefTxnId` helpers remain.

## Files

Slice C behavior present in `224aacd8eb9` and `6c0a5453bf7`:

- `Formats/CasRefSnapshotFormat.h`
- `Formats/CasRefSnapshotFormat.cpp`
- `Formats/CasRefWireVocab.h`
- `Pool/CasRefProtocol.h`
- `Pool/CasRefProtocol.cpp`
- `Tools/CasInspect.cpp`
- `benchmarks/benchmark_cas_ref_protocol.cpp`
- `src/Disks/tests/cas_test_helpers.h`
- `gtest_cas_ref_snapshot_format.cpp`
- `gtest_cas_ref_statemachine.cpp`
- `gtest_cas_ref_writer.cpp`
- `gtest_cas_inspect.cpp`
- `gtest_cas_encoding_pins.cpp`
- `gtest_cas_recovery_streaming.cpp`

The scoped follow-up changes only `Tools/CasInspect.cpp`,
`gtest_cas_inspect.cpp`, this brief, and this report.

## TDD evidence

Raw wire characterization before production codec edits:

- `build_debug/step9_c_raw_characterization_build.log`: build passed.
- `build_debug/step9_c_raw_characterization_exact.log`: all five raw generation-8 tests passed.
- A temporary decoder mutation ignored `rte`/`rts`.
  `build_debug/step9_c_raw_mutation_exact.log` then failed all three retired-field tests.
  The codec was restored byte-for-byte before production edits.

Runtime mutation controls:

- Removing the explicit live assignment built successfully
  (`step9_c_mutation_live_build.log`) and
  `CasRefStateMachine.StateFromSnapshotConstructsLiveState` failed 0/1
  (`step9_c_mutation_live_exact.log`): the observed default was `Removed`.
- Removing the non-live guard built successfully
  (`step9_c_mutation_terminal_build.log`) and
  `CasRefStateMachine.SnapshotOfRefusesTerminalState` failed 0/1
  (`step9_c_mutation_terminal_exact.log`): no exception was thrown.
- After each mutation, the exact pre-mutation `CasRefProtocol.cpp` checksum was restored.

`CasInspect` follow-up:

- With the negative test installed and the stale lifecycle rendering temporarily restored,
  `step9_c_inspect_red_build.log` built and
  `step9_c_inspect_red_exact.log` failed 0/1 because the JSON contained
  `"lifecycle":"Live"`.
- Removing that single rendering line restored the recorded intended-file checksum.

## Final verification

- `build_debug/step9_c_final_build.log`: `unit_tests_dbms` linked successfully; independent log
  review found no warnings or errors.
- `build_debug/step9_c_final_exact.log`: 10/10 passed across five suites, including the five raw
  decoder pins, explicit live reconstruction, terminal snapshot refusal, framing equality,
  lifecycle-free inspection JSON, and the generation-8 byte golden.
- `build_debug/step9_c_final_suite.log`: 99/99 passed across six focused suites.
- `git diff --check` passed for the Slice C paths.

## Coordination note

The combined `224aacd8eb9` commit is not an ideal review boundary: it contains both Slice C and
unrelated namespace-removal/decommission work because another agent committed full shared paths while
the Slice C hunks were present. The final state was therefore audited requirement-by-requirement and
the remaining follow-up is intentionally narrow. Independent review should inspect Slice C semantically
inside `224aacd8eb9` plus `6c0a5453bf7`, then inspect the small follow-up diff separately.
