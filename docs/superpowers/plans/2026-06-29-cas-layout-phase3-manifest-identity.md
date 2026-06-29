---
description: Implementation plan for Phase 3 of the content-addressed layout hot/cold split: durable writer_epoch and per-build manifest ordinal identity.
sidebar_label: CA Layout Phase 3 Manifest Identity
sidebar_position: 20260629
slug: /superpowers/plans/2026-06-29-cas-layout-phase3-manifest-identity
title: CA Layout Phase 3 Manifest Identity
doc_type: plan
---

# CA Layout Phase 3 — Manifest Identity {#ca-layout-phase-3-manifest-identity}

## Goal {#goal}

Replace the current random manifest instance identity with the target identity from the approved hot/cold split
spec:

```text
PartManifestRef = (writer_epoch, build_sequence, manifest_ordinal)
PartManifestId  = (root_namespace, writer_epoch, build_sequence, manifest_ordinal)
```

The object key becomes:

```text
<pool>/cas/manifests/<root_namespace>/<writer_epoch>/<build_sequence>/<000001>.proto
```

This phase has no compatibility requirement. The feature has not shipped in production, so readers do not need to
decode the old string writer token, random `UInt128` instance id, or `<aa>` fanout layout.

## Safety Model {#safety-model}

This phase changes the concrete encoding of the opaque manifest id, not the ownership protocol:

- `PartManifestId` remains namespace-qualified.
- Root journals still store only `PartManifestRef`; the owning namespace is supplied by root context.
- The body still repeats `PartManifestRef` and `root_namespace` and readers still enforce `refMatchesBody`.
- `NoManifestIdReuse` is now by construction:
  `server_root_id` is owner-gated, `writer_epoch` is durable-monotone per `server_root_id`, `build_sequence` is
  monotone per writer epoch, and `manifest_ordinal` is monotone per build.
- The TLA+ model continues to treat manifest ids as opaque values. The required proof action is a targeted
  recheck that the opaque-id abstraction is still valid under allocator uniqueness.

## Current State {#current-state}

- `ManifestRef` is `(writer_instance_id: String, build_sequence: UInt64, manifest_instance_id: UInt128)`.
- `manifestKey` writes `<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto`.
- `Build::stageManifest` mints a random `manifest_instance_id` per staged manifest.
- Sweep and fsck parse manifest keys as `<writer>/<build>/<aa>/<id>.proto`.
- Phase 0 already allocates a durable monotone `writer_epoch` and bridges it through the current string field as
  the suffix after `:`.

## Target State {#target-state}

- `ManifestRef` fields are:
  - `uint64_t writer_epoch`;
  - `uint64_t build_sequence`;
  - `uint32_t manifest_ordinal`.
- `ManifestRefProto` uses the same logical fields and field numbers can be reused because no compatibility is
  required.
- `manifest_ordinal` is rendered as six decimal digits: `000001.proto` through `999999.proto`.
- `Build::stageManifest` increments a per-build ordinal counter and fails closed with `LIMIT_EXCEEDED` after
  `999999`.
- Sweep/fsck parse the final three path segments as `<writer_epoch>/<build_sequence>/<ordinal>.proto`; namespace is
  everything between `cas/manifests/` and those three segments.
- Watermark eligibility compares `ManifestRef.writer_epoch` directly; no string parsing remains.

## Task 1 — Ref Type, Codec, And Layout {#task-1-ref-type-codec-and-layout}

Files:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h`
- `src/Disks/tests/gtest_cas_manifest_id.cpp`
- `src/Disks/tests/gtest_cas_codecs.cpp`

Steps:

1. Replace `ManifestRef` with `writer_epoch`, `build_sequence`, `manifest_ordinal`.
2. Add `manifestOrdinalFileName` rendering `1 -> "000001.proto"` and rejecting `0` / `> 999999` where a key is
   requested.
3. Replace `manifestKey` layout with `<writer_epoch>/<build_sequence>/<ordinal>.proto`.
4. Replace `ManifestRefProto` fields and codec calls.
5. Update ordering/hash tests and root-shard codec round trips.

Verification:

- `ninja -C build unit_tests_dbms > build/build_phase3_task1.log 2>&1`
- `build/src/unit_tests_dbms --gtest_filter='CasManifestId*:CasCodecs*:CasHeaderGolden*' > build/test_phase3_task1.log 2>&1`

## Task 2 — Build Allocator {#task-2-build-allocator}

Files:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp`
- `src/Disks/tests/gtest_cas_build.cpp`

Steps:

1. Add a per-`Build` `next_manifest_ordinal` counter starting at `1`.
2. In `Build::stageManifest`, construct `ManifestRef{writer_epoch, build_sequence, ordinal}` from the store's
   current writer epoch and the build sequence.
3. Keep `putIfAbsentStream` with no preliminary `HEAD`; a precondition failure is still fail-closed because it
   means the allocator tried to reuse a key.
4. Add tests for monotone ordinals, fixed-width key rendering, and the `999999` cap.

Verification:

- `ninja -C build unit_tests_dbms > build/build_phase3_task2.log 2>&1`
- `build/src/unit_tests_dbms --gtest_filter='CasBuild*:CasStore*' > build/test_phase3_task2.log 2>&1`

## Task 3 — GC, Sweep, Fsck, And Exchange {#task-3-gc-sweep-fsck-and-exchange}

Files:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- related tests under `src/Disks/tests/`

Steps:

1. Change `BuildPrefix` to carry `writer_epoch` directly.
2. Change sweep and fsck key parsers to the three-tail-segment format.
3. Remove all `writer_instance_id` string parsing and server-hex extraction from GC/sweep paths.
4. Update diagnostic/event fields that used the old random instance id to use `manifest_ordinal` or the rendered
   `PartManifestId`.
5. Ensure cross-server relink/adopt code stages a fresh receiver-local manifest with a fresh ordinal and never
   copies a sender manifest id.

Verification:

- `ninja -C build unit_tests_dbms > build/build_phase3_task3.log 2>&1`
- `build/src/unit_tests_dbms --gtest_filter='CasGc*:CasOrphanManifestSweep*:CasFsck*:CasProtocolScenarios*:CaWiring*:-CaWiringOps.FreezeViaHardLinksIntoShadow' > build/test_phase3_task3.log 2>&1`

## Task 4 — Global Test Rebaseline And Cleanup {#task-4-global-test-rebaseline-and-cleanup}

Files:

- all touched `src/Disks/tests/gtest_cas_*.cpp`
- `docs/superpowers/worklogs/2026-06-29-cas-layout-hot-cold-split-worklog.md`

Steps:

1. Replace remaining old field names in tests, comments, and assertions.
2. Run the broad CA suite.
3. Record the exact Phase 3 commits and verification in the worklog.
4. Confirm `CasBuild.cpp/.h` and `gtest_cas_build.cpp` handling matches the worklog's deferred-leak note before
   final delivery.

Verification:

- `ninja -C build unit_tests_dbms > build/build_phase3_task4.log 2>&1`
- `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_phase3_task4_broad.log 2>&1`

Expected broad result until the unrelated baseline is fixed: all pass except
`CaWiringOps.FreezeViaHardLinksIntoShadow`.

## Self-Review Checklist {#self-review-checklist}

- No manifest key contains `<aa>` fanout or a random `UInt128` instance id.
- No sweep, fsck, GC, or precommit reclaim path parses server identity from a manifest ref.
- `manifest_ordinal == 0` is never emitted.
- `manifest_ordinal > 999999` fails closed before any object write.
- `refMatchesBody` remains enforced for every manifest body read.
- `PartManifestId` remains namespace-qualified everywhere; no GC map is keyed by `PartManifestRef` alone where
  namespace is required.
- `Build::stageManifest` writes exactly one body per ordinal and does not add a preliminary `HEAD`.
