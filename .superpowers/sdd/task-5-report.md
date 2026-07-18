# Task 5 report: fence decommission slot deletion against successor reclaim

## Implementation

- Captured the victim epoch object's value and token while the decommission admin claim was still held.
- Released the admin claim, then immediately captured and validated the farewell mount written by `finishTeardown` against the claimed epoch.
- Reordered slot retirement to delete `mount`, then `epoch`, then `owner`.
- Used only the captured farewell mount token and under-claim epoch token for the two mutable-object deletes; there is no post-release re-read of their current tokens.
- Classified every `DeleteOutcome` with `classifyDeleteOutcome`. Any result other than `Deleted`, or any backend exception, records a warning, leaves `slot_removed=0`, and stops the tail before the next control object.
- Read and deleted `owner` only after the mount and epoch deletes were both confirmed `Deleted`.
- Added a deterministic backend interleaving with no threads or sleeps. It observes the admin farewell and installs a fresh successor epoch and mount between the retirement tail's observation and exact-token delete.
- Added an uncontended control test proving all three control objects are still removed and `slot_removed=1` is preserved.

## RED evidence

Production code was unchanged for this run.

Command:

```bash
build/src/unit_tests_dbms --gtest_filter='CasDecommission.SuccessorReclaimFencesSlotRetirementTail:CasDecommission.FencedSlotRetirementTailRemovesUncontendedSlot' > build/test_t5_red.log 2>&1
```

Result: exit code 1; 2 tests ran, 1 passed, 1 failed.

Failure:

```text
[  FAILED  ] CasDecommission.SuccessorReclaimFencesSlotRetirementTail
gtest_cas_decommission.cpp:511: report.slot_removed was true, expected false
gtest_cas_decommission.cpp:512: report.warnings.size() was 0, expected 1
```

The uncontended control `CasDecommission.FencedSlotRetirementTailRemovesUncontendedSlot` passed against the pre-fix code.

## GREEN evidence

Build command:

```bash
ninja -C build unit_tests_dbms > build/t5_build.log 2>&1
```

Result: success; `CasDecommission.cpp`, `libdbms.a`, and `src/unit_tests_dbms` built without warnings or errors.

Focused test command:

```bash
build/src/unit_tests_dbms --gtest_filter='CasDecommission.SuccessorReclaimFencesSlotRetirementTail:CasDecommission.FencedSlotRetirementTailRemovesUncontendedSlot' > build/test_t5_green.log 2>&1
```

Result: exit code 0; 2 tests ran, 2 passed, 0 failed (1 ms).

## Full unit battery

Command:

```bash
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*' > build/test_t5_battery.log 2>&1
```

Result: exit code 0; 922 tests from 162 suites ran, 922 passed, 0 failed, 0 skipped (36.187 s). The count is the 920-test baseline plus the two new tests.

## Files changed

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp`
- `src/Disks/tests/gtest_cas_decommission.cpp`
- `.superpowers/sdd/task-5-report.md`

## Concerns

No implementation or test concerns remain. The worktree contained unrelated pre-existing modified and untracked files; they were not changed or included in this task.

## Commit status

The requested signed-off commit could not be created because this execution sandbox mounts `.git` read-only. The first staging attempt failed with:

```text
fatal: Unable to create '/home/mfilimonov/workspace/ClickHouse/master/.git/index.lock': Read-only file system
```

The source changes and this report remain in the working tree. In an environment with writable Git metadata, stage only the three files listed above and run:

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp \
    src/Disks/tests/gtest_cas_decommission.cpp
git add -f .superpowers/sdd/task-5-report.md
git commit -s -m 'cas: decommission — fence slot deletes against successor reclaim, inspect every DeleteOutcome (triage #9)'
```

## Fix round 2

### Task A: final successor liveness recheck

- Added a final fresh `get` of both the victim `mount` and `epoch` objects after their exact-token
  deletes succeed and immediately before reading `owner`.
- If either mutable object exists, or either liveness read throws, retirement fails closed: `owner`
  is not read or deleted, `slot_removed` remains false, and the report contains a warning. The
  successor-presence warning is `slot delete aborted: successor reappeared after mutable
  control-object deletion; owner kept`.
- Added deterministic test
  `CasDecommission.SuccessorReclaimAfterEpochDeleteKeepsOwnerAnchor`. Its backend hook recreates
  fresh `epoch` and `mount` objects synchronously after the stale epoch delete succeeds and before
  decommission can read `owner`. The test checks that the original owner token and bytes remain
  unchanged, no owner delete is attempted, both successor objects survive with their fresh tokens
  and bytes, `slot_removed` is false, and a warning is present.

### RED evidence

Production code was unchanged for this run; only the new test and hook were present.

```bash
ninja -C build unit_tests_dbms > build/t5fix2_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasDecommission.SuccessorReclaimAfterEpochDeleteKeepsOwnerAnchor' > build/t5fix2_red.log 2>&1
```

The build succeeded. The test exited 1: one test ran and failed. The current code reported
`slot_removed=true`, produced no warning, attempted one owner delete, and left `owner` absent. These
four assertions failed exactly as expected.

### GREEN evidence

```bash
ninja -C build unit_tests_dbms > build/t5fix2_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasDecommission.SuccessorReclaimAfterEpochDeleteKeepsOwnerAnchor:CasDecommission.FencedSlotRetirementTailRemovesUncontendedSlot:CasDecommission.SuccessorReclaimFencesSlotRetirementTail' > build/t5fix2_targeted.log 2>&1
```

The build succeeded with no actionable warnings or errors. All three focused tests passed (3/3,
zero failures): the new late-successor test, the existing uncontended tail test, and the existing
successor-before-mount-delete test.

### Task B: farewell-token capture investigation

Stopped without implementation under the task's escape hatch. The token authored by the farewell
`putOverwrite` exists only inside `MountLeaseKeeper::terminate`, which records it into the private
`SingleWriterSlot::last_token`. A direct capture cannot be implemented solely in `CasPool.h/.cpp`,
`CasServerRoot.cpp`, and `CasDecommission.cpp`. It would require:

- changing the terminal-operation contract in `CasServerRoot.h/.cpp` (`terminate`, `doTerminate`,
  and `MountLeaseKeeper::stop`) to return or expose the authored token;
- propagating the result through `CasMountRuntime.h/.cpp::finishTeardown`;
- adding an explicit, idempotent partial-teardown state to `Pool`, so a public retirement method can
  drain and finish teardown while `~Pool` reliably skips duplicate teardown across success, backend
  failure, no-keeper, read-only, and partially completed paths; and
- defining how the existing destructor-only exception suppression maps to a missing token returned
  to decommission.

This changes shared lifecycle APIs and semantics across more files than the permitted low-risk
addition. A dedicated follow-up should design and test it rather than partially refactor teardown in
this fix round.

### Full battery

```bash
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*' > build/t5fix2_battery.log 2>&1
```

Result: exit code 0; 924 tests from 162 suites ran, 924 passed, zero failed (36.228 s). Two disabled
tests were reported; no skipped count was shown. Exception traces in the log belong to passing
negative-path tests.

### Files changed

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp`
- `src/Disks/tests/gtest_cas_decommission.cpp`
- `.superpowers/sdd/task-5-report.md`

### Concerns

- The mandated final checks are separate backend reads, not a transaction with owner deletion. They
  close the deterministic epoch-delete-to-owner-read window covered here, but a successor could in
  principle begin after both absence reads and before the owner exact-delete. Fully serializing
  successor startup with owner retirement would require a stronger cross-object protocol, such as a
  successor-visible owner generation/touch or transactional backend primitive.
- The worktree contains many unrelated pre-existing modified and untracked files. They were not
  changed for this fix round and must not be included in its commit.
