# Codex review round 2 of spec rev.2 (gpt-5.6-sol, high) {#codex-review-round-2}

Reviewed commit: d4628611a66. Verdict: REVISE. Findings verbatim:

## Round-1 status

- [CRITICAL] First-LIST race — **RESOLVED** — §3.2 adds start-and-settle version bumps and test 6. New concurrency defects remain below.
- [MAJOR] Fence/lifetime — **RESOLVED** — §3.2 requires fresh admission and generation equality before every hit; test 8 covers loss/remount.
- [MAJOR] Runtime lifetime/weight — **PARTIALLY** — §§2, 3.2 correctly weaken the promise to a resident runtime and add `namespace_file_names_bytes`, but budget enforcement remains incomplete.
- [MAJOR] Path compatibility — **RESOLVED** — §3.1 preserves `tableSubdirExists`/`tableSubdirChildren` or generic-tree fall-through when the ref is unresolved; test 3 covers it.
- [MAJOR] Nested-directory compatibility contradiction — **RESOLVED** — §2 explicitly identifies resolved-part nested directories as the sole deliberate answer change.
- [MINOR] Single-writer wording — **RESOLVED** — §§1, 3.2 distinguish one admitted mount from concurrent threads and restrict janitor mutation to dead lives.
- [MAJOR] Missing concurrency/lifetime/destructive tests — **RESOLVED** — §4 tests 5–11 add all requested cases.
- [MINOR] Missing routing coverage — **RESOLVED** — §4 tests 1 and 3 add moving, shadow, non-Atomic, missing-ref and collision coverage.
- [MINOR] Non-executable integration bound/placeholders — **RESOLVED** — §4 names `test_cas_directory_probes`, disables GC, and asserts equality/zero deltas in tests 12–13.
- [MINOR] Wrong physical path — **RESOLVED** — §1 uses `cas/ns/state/<life_id>/_files/`.
- [NIT] Death-test wording — **RESOLVED** — §4 now states that no `LOGICAL_ERROR` is introduced and specifies the ASan lane.

## Simpler alternatives

None found.

- For §3.1, `ContentAddressedMetadataStorage::{classifyDirectory,existsDirectory,listDirectory}` need the same ref-resolution and unresolved-ref fall-through; `DirShape::PartFile` is the smallest coherent shared route.
- For §3.2, `CasRefLedger::RefTableRuntime` remains the smallest lifecycle-safe cache location. The protocol needs strengthening below, not relocation.

## New findings

Unresolved-ref fall-through preserves current successful answers: Atomic paths reuse the old table-subdirectory logic, while non-Atomic part-shaped paths with no `TableFilePath` reuse `liveTreeDirHasChildren`/`listLiveTreeChildren`. If the preliminary ref lookup fails, the exception propagates, consistent with the no-fallback rule.

The hit-generation comparison is meaningful and should not refuse forever on a healthy mount. `mount_requests` and `CasRefLedger` use the same `mount_runtime.fenceGeneration` source (`Pool/CasPool.cpp:179-185,230-236`); runtimes validate that generation at creation (`Pool/CasRefLedger.cpp:568-600`), and production remount clears them before rearming (`Pool/CasPool.cpp:1497-1557`).

Test 6 is implementable without a new production hook: subclass `CountingBackend`, override `write`, call the base implementation to land the object, then invoke a callback or wait on `ManualBarrier` before returning (`src/Disks/tests/cas_test_helpers.h:128-159,1720-1785`). Existing tests already use this override pattern.

- [CRITICAL] §3.2 write-through — Concurrent PUT and DELETE can leave the set permanently stale because callbacks are applied in call-settlement order, not durable-object order. Example: PUT replaces an object and pauses after the backend write; DELETE observes and removes that replacement and settles first; PUT then settles and inserts the name. Storage is absent, cache says present. Reversing the durable order produces storage present/cache absent, allowing rename to omit a file before dropping the source (`Pool/CasPlainObjects.cpp:44-75`; `Backend/CasHotKeys.cpp:62-143`; `Backend/CasRequests.cpp:621-654`; `ContentAddressedTransaction.cpp:1293-1303`). If a write observes any other version transition after its start, forget the set instead of applying its callback, or serialize updates in durable order. Add concurrent PUT/DELETE tests for both landing orders.

- [CRITICAL] §3.2 read/install protocol — `installNamespaceFileNames(life, version, names)` lacks exact runtime identity. A LIST may capture runtime A/version 0, runtime A may be evicted or cleared by remount, and runtime B for the same life may be created with version 0; the old LIST then installs into B. Generation rejects remount replacement only if installation checks it, which the specified method does not, and it cannot distinguish same-generation eviction/recreation. A delayed namespace-file write may also run while no runtime exists because write buffers retain only `life` (`ContentAddressedTransaction.cpp:838-851`). `runtime_id` already exists for exactly this ABA problem (`Pool/CasRefLedger.h:746-765`); return an opaque runtime token from the lookup and require matching runtime ID, life, generation and version at install. Specify that a write which began without a runtime updates or invalidates any runtime materialized before it settles.

- [MAJOR] §§3.2, 4 test 9 — Adding `namespace_file_names_bytes` to `weightOf` does not make growth enforce the budget. `enforceRefTableCacheBudget` is called only after ref-table recovery (`Pool/CasRefLedger.cpp:1671-1675`), not after installing or growing the name set, and it protects `keep_ns` from eviction (`:1736-1813`). A large set on a warm or sole runtime can therefore exceed the budget indefinitely, so the claim that it “cannot escape” and the oversized-set portion of test 9 are not specified implementably. Define enforcement after install/write-through, including how the just-touched runtime can shed the set or be evicted, and define a conservative byte formula including set-node/string allocation overhead.

- [MINOR] §3.2 lines 158–159 — The claim that two bumps ensure an overlapping LIST is “never installed” is false. A write can start, a LIST can record the post-start version and install before the write settles; the settle bump later repairs or forgets it. Either weaken this to “cannot remain installed after the write settles” or track in-flight writes and reject installation while the count is nonzero.

- [MINOR] §4 test 5 — “Either already contains the name or issues a LIST” does not fully state the oracle. After the hooked PUT has returned, the second call must return a set containing the name; only its request count may vary.

- [NIT] §3.2 read path — `requests.admit()` is an unresolved identifier in `Pool`; name the intended `mount_requests.admit()` explicitly.

Verdict: REVISE
