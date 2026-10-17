# Codex review round 3 of spec rev.3 (gpt-5.6-sol, high) {#codex-review-round-3}

Reviewed commit: f432676c515. Verdict: REVISE. Findings verbatim:

## Round-2 status

- [CRITICAL] Concurrent PUT/DELETE settlement — RESOLVED — §3.2 “Write path” serializes LIST, PUT and DELETE through `namespace_files_mutex`; §4 test 7 covers both orders.
- [CRITICAL] Exact runtime identity/write without runtime — RESOLVED — §3.2 “Where the names live” pins the captured runtime, and “A write that begins without a runtime” adds post-settlement invalidation. A new remount transition defect remains below.
- [MAJOR] Cache-budget enforcement — NOT ADDRESSED — §3.2 “Memory” explicitly excludes the names from `ref_table_cache_bytes` and introduces no cap.
- [MINOR] False two-version-bump claim — RESOLVED — §3.2 removes that protocol and orders operations with the mutex.
- [MINOR] Incomplete test oracle — RESOLVED — §4 tests 5–6 require the final answer to contain the name without another LIST.
- [NIT] Unresolved `requests.admit()` — RESOLVED — §3.2 “Read path” names `mount_requests.admit()`.

## Simpler alternatives

- [MINOR] §3.2 — A smaller design exists: put one dedicated `namespace_files_mutex` in `CasRefLedger`, not one in every `RefTableRuntime`. Have `Pool::{list,put,remove}NamespaceFile` hold it across lookup, request and cache update; after every settled write, freshly look up the current runtime and update/reset it. Because no cache-populating LIST can overlap any write, the special write-without-runtime protocol and remount ABA reasoning disappear. It serializes namespace-file I/O across tables, but §2 constrains request counts and answers, not cross-table concurrency.

## New findings

- [CRITICAL] §3.2 remount transition — The guard pins the object against budget eviction, but not against remount detachment. `quiesceRefTablesForRemount` clears every slot without taking `namespace_files_mutex` (`Pool/CasRefLedger.cpp:1817-1868`), and remount then rearms (`Pool/CasPool.cpp:1514-1557`). An old-generation PUT/DELETE can hold runtime A’s mutex, runtime B can be created and cache a LIST, and then the old request can land and update/reset only detached A. Runtime B remains stale, allowing removal or rename to omit files (`ContentAddressedTransaction.cpp:1164-1166,1293-1303`). The shared pointer proves only non-eviction under the `use_count() != 1` check (`Pool/CasRefLedger.cpp:1764-1809`). Also specify whether the guard’s `CasOperation` is reused: `CasPlainObjects` currently admits its own operation (`Pool/CasPlainObjects.cpp:18-23,40-41,58-65`), so guard generation and request generation can differ.

- [MAJOR] §3.2 “Memory” — The claimed bound is not an invariant. Namespace files are public `IDisk` state and mutation-file names have no specified cap; each `std::set` node also costs substantially more than its string. These bytes bypass the documented 256 MiB resident-cache ceiling (`Pool/CasPool.h:311-321`) and `weightOf` (`Pool/CasRefLedger.cpp:1751-1760`). Account and enforce growth, or cap only this optional cache and leave it unpopulated when oversized so reads continue using real LISTs.

- [MAJOR] §4 test 8 — The stated landed-then-throw backend does not reach the failure rule. After an ambiguous PUT, the request engine exact-reads the object and recognizes its bytes as committed; after an ambiguous DELETE, the read loop retries and observes `Gone` (`Backend/CasRequests.cpp:1039-1069`; `Pool/CasPlainObjects.cpp:9-23,35-41`). Specify continued resolve/retry failure through an injected policy deadline, then assert that the public call throws and the same-generation next read LISTs. `CountingBackend` plus `FakeClock` can implement that stronger fault.

- [MAJOR] §4 test 12 — Its ordering is impossible with `CountingBackend`: if PUT calls the base implementation and blocks “after landing,” a subsequent strongly consistent LIST includes the name, so it cannot install a set “without the name.” Block PUT before the base `write`, let the newly created runtime LIST and install absence, then release PUT to land and exercise post-settlement invalidation (`src/Disks/tests/cas_test_helpers.h:128-159,1720-1785`).

- [MINOR] §3.2 “Cost of the mutex” — “The callers already serialize these writes” is too broad. `MergeTreeDeduplicationLog` does hold `state_mutex` across finalize/rotation/removal (`MergeTreeDeduplicationLog.cpp:247-355`), but delayed namespace-file buffers can finalize on another thread and retain only the life (`ContentAddressedTransaction.cpp:805-852`). The new mutex supplies the general serialization; caller serialization is not a valid general premise.

No conventional lock cycle was found with recovery, eviction, remount, the hot-key lane, or the inspected callers: those paths do not acquire `namespace_files_mutex` in reverse order. Tests 5–7 are implementable with `ManualBarrier` and `CountingBackend`; tests 8 and 12 require the corrections above. No other placeholder remains.

REVISE
