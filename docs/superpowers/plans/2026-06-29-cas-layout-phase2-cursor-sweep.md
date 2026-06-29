---
description: Implementation plan for Phase 2 of the content-addressed layout hot/cold split: cursor-paced bounded orphan part-manifest sweep.
sidebar_label: CA Layout Phase 2 Cursor Sweep
sidebar_position: 20260629
slug: /superpowers/plans/2026-06-29-cas-layout-phase2-cursor-sweep
title: CA Layout Phase 2 Cursor Sweep
doc_type: plan
---

# CA Layout Phase 2 — Cursor-Paced Orphan Part-Manifest Sweep {#ca-layout-phase-2-cursor-paced-orphan-part-manifest-sweep}

## Goal {#goal}

Replace the current per-round `pickOneSweepTarget` scan with a cursor-paced bounded pass over `cas/manifests/`.
The sweep remains a best-effort debris cleanup path: it must not affect blob reachability, `fold`, `retire`, or
`recheck`. Losing cursor progress is allowed and only causes repeated scan work.

This phase is identity-preserving. It does not change `ManifestRef`, `ManifestId`, part-manifest keys, or TLA+
reachability invariants.

## Current State {#current-state}

- `Gc::runRegularRound` runs the orphan sweep after `recheck` and `trim`.
- `pickOneSweepTarget` reads `gc/registry`, lists each namespace's `cas/manifests/<ns>/`, parses
  `<writer_instance_id>/<build_sequence>/...`, and returns the first eligible build prefix.
- `sweepNamespace` then lists that whole build prefix and deletes every inactive object under it by exact token.
- This is safe, but a quiet round can still scan many namespace manifest prefixes to find one eligible prefix.

## Target Protocol {#target-protocol}

One GC round does at most:

- one `LIST cas/manifests/` page after `GcState.manifest_sweep_cursor`, bounded by
  `PoolConfig.manifest_sweep_list_budget_keys`;
- at most `PoolConfig.manifest_sweep_delete_budget_keys` exact-token deletes;
- one best-effort `gc/state` CAS to persist the new cursor.

Cursor semantics follow `Backend::list`: `next_cursor` is the last returned key and the next call resumes
strictly after it. Empty cursor means start from the beginning.

The safety authority is unchanged:

- build-death comes from `prefixEligible`;
- owner protection comes from the same committed/live-precommit/pending-removal view used by
  `activeManifestKeys`;
- deletion is `deleteExact(listed.token)` when LIST returned a token, otherwise `HEAD` then `deleteExact`;
- malformed or ambiguous keys are skipped, never guessed.

## Design Decisions {#design-decisions}

- Keep `manifest_sweep_cursor` in `GcState`, but treat it as non-load-bearing state.
- Persist the cursor with a separate lease-guarded `gc/state` CAS after the safety-critical round already
  completed. If the CAS loses, cursor progress is discarded and the next round rescans.
- Keep the existing `sweepNamespace` API during the transition for tests that exercise the liveness predicate
  directly, but stop calling `pickOneSweepTarget` from production GC once the cursor pass lands.
- Do not make LIST tokens mandatory. `ObjectStorageBackend::list` surfaces them when the provider does; fallback
  to `HEAD` is correct and only costs an extra metadata read.

## Task 1 — Format And Config {#task-1-format-and-config}

Files:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- `src/Disks/tests/gtest_cas_gc_formats.cpp`

Steps:

1. Add `manifest_sweep_cursor` to `GcState` and `GcStateProto`.
2. Add `PoolConfig.manifest_sweep_list_budget_keys` and
   `PoolConfig.manifest_sweep_delete_budget_keys` with conservative defaults.
3. Thread optional disk config values through `ContentAddressedMetadataStorage` so production can tune the budgets.
4. Add round-trip tests proving the cursor survives `encodeGcState` / `decodeGcState` and defaults to empty for
   older in-memory test objects.

Verification:

- `ninja unit_tests_dbms > build_phase2_task1.log 2>&1`
- `./src/unit_tests_dbms --gtest_filter='CasGcFormats*:CasStore*:CaWiring*:-CaWiringOps.FreezeViaHardLinksIntoShadow' > test_phase2_task1.log 2>&1`

## Task 2 — Cursor Page Sweep API {#task-2-cursor-page-sweep-api}

Files:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp`
- `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp`

New API shape:

```cpp
struct ManifestSweepResult
{
    String next_cursor;
    bool wrapped = false;
    uint64_t listed = 0;
    uint64_t deleted = 0;
    uint64_t skipped = 0;
};

ManifestSweepResult sweepManifestCursorPage(
    Store & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t delete_budget);
```

Steps:

1. Parse listed keys under `layout.casManifestsPrefix()` into `(RootNamespace, BuildPrefix, object_key)`.
2. Reuse `prefixEligible` and `activeManifestKeys`; do not duplicate liveness logic.
3. Delete by listed token when available; otherwise `HEAD` and `deleteExact`.
4. Stop deleting once `delete_budget` is exhausted, but still return the page cursor from the bounded LIST page.
5. If the page ends, return `next_cursor=""` and `wrapped=true`.
6. Add tests for:
   - cursor advances and wraps;
   - list budget is respected;
   - delete budget is respected;
   - malformed keys are skipped;
   - committed/live-precommit/pending-removal objects are spared;
   - eligible unowned debris is deleted.

Verification:

- `ninja unit_tests_dbms > build_phase2_task2.log 2>&1`
- `./src/unit_tests_dbms --gtest_filter='CasOrphanManifestSweep*:CasBackendContract*:CasBackend*' > test_phase2_task2.log 2>&1`

## Task 3 — GC Tail Integration {#task-3-gc-tail-integration}

Files:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h`
- `src/Disks/tests/gtest_cas_gc_round.cpp`
- `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp`

Steps:

1. Replace the `pickOneSweepTarget` / `sweepNamespace` call in `Gc::runRegularRound` with
   `runManifestSweepCursorPass`.
2. Read the just-current `GcState` and token after `trim`.
3. Run `sweepManifestCursorPage` with budgets from `PoolConfig`.
4. Attempt a best-effort `gc/state` CAS that updates only `manifest_sweep_cursor`.
5. If the CAS loses, log at debug/info and discard cursor progress. Do not throw unless the state object is corrupt.
6. Keep exceptions from the sweep fail-open, matching the current warning-and-skip behavior.

Verification:

- `ninja unit_tests_dbms > build_phase2_task3.log 2>&1`
- `./src/unit_tests_dbms --gtest_filter='CasGc*:CasOrphanManifestSweep*:CasRetireView*:CasFsck*' > test_phase2_task3_gc.log 2>&1`
- `./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > test_phase2_task3_broad.log 2>&1`

Expected broad result until the unrelated baseline is fixed: all pass except the known
`CaWiringOps.FreezeViaHardLinksIntoShadow`.

## Task 4 — Remove Old Production Picker Path {#task-4-remove-old-production-picker-path}

Files:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp`
- `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp`
- `docs/superpowers/worklogs/2026-06-29-cas-layout-hot-cold-split-worklog.md`

Steps:

1. Remove `pickOneSweepTarget` if no tests or diagnostics need it.
2. Keep `sweepNamespace` only if it remains useful as a narrow liveness-predicate test seam; otherwise move those
   tests to `sweepManifestCursorPage`.
3. Update comments from namespace-prefix sweep to cursor-paced `cas/manifests/` sweep.
4. Update the worklog with the Phase 2 commits and verification.

Verification:

- `ninja unit_tests_dbms > build_phase2_task4.log 2>&1`
- `./src/unit_tests_dbms --gtest_filter='CasOrphanManifestSweep*:CasGc*:CasLayout.*' > test_phase2_task4.log 2>&1`

## Self-Review Checklist {#self-review-checklist}

- The sweep cursor is never read by `fold`, `retire`, `fence`, `recheck`, `trim`, `previewDeletes`, or
  `RetireView`.
- `GcState.manifest_sweep_cursor` can be empty, stale, reset, or lost without changing reachability.
- A missing LIST token does not delete anything without a fresh `HEAD`.
- A malformed manifest key does not throw and does not delete.
- A missing or future watermark does not delete.
- The active-owner protection logic is shared with the old sweep path or tested byte-for-byte equivalently.
- `CasBuild.cpp/.h` and `gtest_cas_build.cpp` remain untouched.
