# Codex review round 10 of spec rev.11 (gpt-5.6-sol, high) {#codex-review-round-10}

Reviewed commit: 61f5dec45cf (rev.11). Verdict: REVISE (wording scope and test oracles only; no design defect). The review loop for this spec ends with rev.12. Findings verbatim:

## Round-9 status
- [MINOR] Smaller `.proj`-only upstream alternative — RESOLVED.
- [MAJOR] Unconditional no-request claim — PARTIALLY; narrowed to LIST, but “no LIST, ever” remains overbroad.
- [MINOR] `isDirectoryEmpty` answer change and coverage — RESOLVED.
- [MINOR] Detached/moving and non-Atomic coverage — RESOLVED.
- [MINOR] Exact unresolved-ref oracle — RESOLVED.
- [MAJOR] Test 4 instrumentation seam — PARTIALLY; the correct layer is named, but the existing recorder cannot identify LIST operations.
- [MINOR] Test 5 merge/cleanup stabilization — PARTIALLY; merge configuration is offered, but cleanup is still timing-dependent.
- [MINOR] Tests 2/4 cache-sensitive GET expectations — PARTIALLY; test 2 still has no executable cold-cache oracle.
- [MINOR] Acceptance conflated LIST and GET behavior — RESOLVED.
## New findings
- [MAJOR] §3.1 — “no LIST, ever” contradicts the unresolved-ref branch, which deliberately falls through to `tableSubdirExists`/`liveTreeDirHasChildren`, and test 3 expects one LIST. Scope the guarantee to a successfully resolved `PartFile`. `docs/superpowers/specs/2026-09-26-cas-directory-probes-no-list-design.md:118,134,180-185`.
- [MINOR] §3.1 — “A manifest GET is a per-part request that the load already issued once” is false when both retained-view and manifest-decode caches are disabled, oversized, or evicted: every probe can GET the manifest. `docs/superpowers/specs/2026-09-26-cas-directory-probes-no-list-design.md:131-137`; `Parts/PartFolderAccess.cpp:154-186,235-266`; `Pool/CasManifestReader.cpp:43-57`.
- [MINOR] §2/§4 — The `isDirectoryEmpty == false` exception needs to say “non-projection nested directory.” `ProjectionDir` remains deliberately empty even when it lists entries, through the special case at `ContentAddressedMetadataStorage.cpp:1939-1945`. Test 2 should preserve that existing assertion. Spec: `:59-63,174-176`.
- [MAJOR] §4, tests 2–3 — The stated new test “over `CountingBackend`” cannot exercise `ContentAddressedMetadataStorage`: it accepts an `ObjectStoragePtr` and constructs `ObjectStorageBackend` internally. Thus the new `listCount` assertions remain unimplementable as specified. Use an operation-counting `IObjectStorage` wrapper for tests 2–4 or define an explicit injection seam. Spec: `:166-185`; `ContentAddressedMetadataStorage.h:140-157`; `ContentAddressedMetadataStorage.cpp:819-835`.
- [MAJOR] §4, test 4 — Its scope and oracle conflict. A real table load legitimately LISTs `_files/` once from `TableDir`, so a full-load assertion of no such LIST is wrong. If this is only the checksum-probe phase, say so, reset recording after table enumeration/setup, and assert zero additional LISTs. The existing recorder stores only paths, not operation kinds, so “key set contains no LIST” is not expressible without strengthening the seam. Spec: `:186-191`; `ContentAddressedMetadataStorage.cpp:1842-1855`; `gtest_cas_namespace_file_request_profile.cpp:249-373`.
- [MAJOR] §4/§5, test 2 — “May GET” is not an oracle, yet acceptance says GET behavior is covered. `part_folder_cache_bytes = 0` alone does not force a GET because `manifest_decode_cache_bytes` defaults to 128 MiB. Specify cache settings, warming/reset boundaries, and exact GET expectations, or remove the coverage claim. Spec: `:177-179,207-208`.
- [MAJOR] §4, test 5 — Exact equality remains timing-dependent. `SYSTEM STOP MERGES` is an in-memory action lock and does not persist across restart; `gc_enabled = 0` also does not stop startup/background `MergeTree` temporary-directory cleanup. Require the merge setting from table creation and lengthen cleanup intervals beyond the test, rather than relying on an immediate read. Spec: `:193-201`; `StorageMergeTree.cpp:289-313`; `ActionLocksManager.cpp:33-48`.
- [MINOR] §2/§4 — No test distinguishes an absent ref from a failed ref/manifest request. Add fault injection proving the exception propagates and the old LIST branch is not entered. Spec: `:64,180-185`.
REVISE
## Round-9 status
- [MINOR] Smaller `.proj`-only upstream alternative — RESOLVED.
- [MAJOR] Unconditional no-request claim — PARTIALLY; narrowed to LIST, but “no LIST, ever” remains overbroad.
- [MINOR] `isDirectoryEmpty` answer change and coverage — RESOLVED.
- [MINOR] Detached/moving and non-Atomic coverage — RESOLVED.
- [MINOR] Exact unresolved-ref oracle — RESOLVED.
- [MAJOR] Test 4 instrumentation seam — PARTIALLY; the correct layer is named, but the existing recorder cannot identify LIST operations.
- [MINOR] Test 5 merge/cleanup stabilization — PARTIALLY; merge configuration is offered, but cleanup is still timing-dependent.
- [MINOR] Tests 2/4 cache-sensitive GET expectations — PARTIALLY; test 2 still has no executable cold-cache oracle.
- [MINOR] Acceptance conflated LIST and GET behavior — RESOLVED.
## New findings
- [MAJOR] §3.1 — “no LIST, ever” contradicts the unresolved-ref branch, which deliberately falls through to `tableSubdirExists`/`liveTreeDirHasChildren`, and test 3 expects one LIST. Scope the guarantee to a successfully resolved `PartFile`. `docs/superpowers/specs/2026-09-26-cas-directory-probes-no-list-design.md:118,134,180-185`.
- [MINOR] §3.1 — “A manifest GET is a per-part request that the load already issued once” is false when both retained-view and manifest-decode caches are disabled, oversized, or evicted: every probe can GET the manifest. `docs/superpowers/specs/2026-09-26-cas-directory-probes-no-list-design.md:131-137`; `Parts/PartFolderAccess.cpp:154-186,235-266`; `Pool/CasManifestReader.cpp:43-57`.
- [MINOR] §2/§4 — The `isDirectoryEmpty == false` exception needs to say “non-projection nested directory.” `ProjectionDir` remains deliberately empty even when it lists entries, through the special case at `ContentAddressedMetadataStorage.cpp:1939-1945`. Test 2 should preserve that existing assertion. Spec: `:59-63,174-176`.
- [MAJOR] §4, tests 2–3 — The stated new test “over `CountingBackend`” cannot exercise `ContentAddressedMetadataStorage`: it accepts an `ObjectStoragePtr` and constructs `ObjectStorageBackend` internally. Thus the new `listCount` assertions remain unimplementable as specified. Use an operation-counting `IObjectStorage` wrapper for tests 2–4 or define an explicit injection seam. Spec: `:166-185`; `ContentAddressedMetadataStorage.h:140-157`; `ContentAddressedMetadataStorage.cpp:819-835`.
- [MAJOR] §4, test 4 — Its scope and oracle conflict. A real table load legitimately LISTs `_files/` once from `TableDir`, so a full-load assertion of no such LIST is wrong. If this is only the checksum-probe phase, say so, reset recording after table enumeration/setup, and assert zero additional LISTs. The existing recorder stores only paths, not operation kinds, so “key set contains no LIST” is not expressible without strengthening the seam. Spec: `:186-191`; `ContentAddressedMetadataStorage.cpp:1842-1855`; `gtest_cas_namespace_file_request_profile.cpp:249-373`.
- [MAJOR] §4/§5, test 2 — “May GET” is not an oracle, yet acceptance says GET behavior is covered. `part_folder_cache_bytes = 0` alone does not force a GET because `manifest_decode_cache_bytes` defaults to 128 MiB. Specify cache settings, warming/reset boundaries, and exact GET expectations, or remove the coverage claim. Spec: `:177-179,207-208`.
- [MAJOR] §4, test 5 — Exact equality remains timing-dependent. `SYSTEM STOP MERGES` is an in-memory action lock and does not persist across restart; `gc_enabled = 0` also does not stop startup/background `MergeTree` temporary-directory cleanup. Require the merge setting from table creation and lengthen cleanup intervals beyond the test, rather than relying on an immediate read. Spec: `:193-201`; `StorageMergeTree.cpp:289-313`; `ActionLocksManager.cpp:33-48`.
- [MINOR] §2/§4 — No test distinguishes an absent ref from a failed ref/manifest request. Add fault injection proving the exception propagates and the old LIST branch is not entered. Spec: `:64,180-185`.
REVISE
