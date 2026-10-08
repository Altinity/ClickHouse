# Codex review round 4 of spec rev.4 (gpt-5.6-sol, high) {#codex-review-round-4}

Reviewed commit: b63049f0b2f. Verdict: REVISE. Findings verbatim:

## Round-3 status

- [CRITICAL] Remount transition — RESOLVED — §3.2 “Post-settle rule” resets the current successor runtime after the original guard is released.
- [MAJOR] Cache-budget enforcement — PARTIALLY — §3.2 “Memory” caps each set at 1 MiB, but aggregate namespace-name cache memory remains outside `ref_table_cache_bytes`.
- [MAJOR] Test 8 ambiguous writes — RESOLVED — §4 test 8 keeps the fault active through the retry deadline; the DELETE wording needs correction below.
- [MAJOR] Test 12 ordering — RESOLVED — §4 test 12 now blocks PUT before the base `write`, allowing the intervening LIST to install absence.
- [MINOR] Caller-serialization premise — RESOLVED — §3.2 “Cost of the mutex” now identifies the new mutex as the general serialization mechanism.

## Simpler alternatives

- [MINOR] §3.2 — The ledger-wide-mutex rejection is sound operationally. Every table-level write buffer finalizes through `putNamespaceFile`, including each dedup-log rewrite (`ContentAddressedTransaction.cpp:825-851`), so holding one mutex across S3 retries would serialize inserts across all deduplicating tables.
- [MINOR] §3.2 — A smaller alternative is a fixed bank of striped mutexes in `CasRefLedger`, indexed by the complete `NamespaceLifeId`. Hold the stripe across runtime lookup, LIST/PUT/DELETE, and final cache update/reset. The lock survives eviction/remount without lifecycle bookkeeping; successor LISTs cannot overlap the write; unrelated tables serialize only on hash collision. This removes the per-runtime mutex and special two-runtime locking protocol while meeting §2.

## New findings

- [MAJOR] §3.2 “Where the names live” / “Post-settle rule” — The stated API cannot perform the post-settle rule. The spec says the ledger exposes only `lockNamespaceFiles(life, fence_generation)`, but settlement must access the current runtime irrespective of the original generation after releasing that guard. The only existing lookup is private (`Pool/CasRefLedger.h:1034-1036`; `Pool/CasRefLedger.cpp:561-565`). Specify a second ledger operation such as `resetCurrentNamespaceFilesIfDifferent(life, prior_runtime_id)`, including exact-life checking and shared-pointer pinning, or put equivalent settlement behavior into the guard API. Reusing the original generation-bound lookup would reopen the remount defect.

- [MAJOR] §3.2 “Memory” — The cap is per runtime, not global. With N resident runtimes, retained namespace names may consume approximately N MiB while remaining invisible to the documented 256 MiB ceiling (`Pool/CasPool.h:311-321`; `Pool/CasRefLedger.cpp:1751-1760`). Runtimes with small or zero ref-state weight make this effectively unbounded. Add a global namespace-name cache budget or include these estimates in global accounting and shed populated sets when enforcement runs.

- [MAJOR] §§2, 3.2, 5, 6 — The cap contradicts the unconditional warm-node guarantee. An oversized table never installs its set, so every `clearOldTemporaryDirectories` pass LISTs it, contrary to §2 and acceptance #2; §6 likewise says names are listed once per resident runtime. Qualify those statements with the cap exception, or choose an eviction policy that preserves the stated guarantee.

- [MINOR] §4 test 8 — DELETE does not use the PUT settle-by-read path. `removeCurrent` retries `remove` and normally treats the resulting `Gone` as success (`Backend/CasRequests.cpp:545-654`). The injected backend must land the first delete and then throw after every later `remove`, including attempts whose base result is `Gone`, until the virtual deadline.

Post-settle interleaving audit: no further stale-set schedule or lock cycle was found, provided the missing ledger operation pins the successor with a `shared_ptr` and the original guard is released before successor locking. Eviction cannot remove that pinned successor; a runtime created after settlement can only LIST the settled state; repeated remounts reduce to the same case. Remount itself does not acquire the namespace mutex (`Pool/CasRefLedger.cpp:1817-1868`).

Tests 8, 12, 13 and 14 are implementable with the existing helpers:

- Test 8: `CountingBackend` subclass plus `FakeClock`/the request clock seams (`cas_test_helpers.h:259-275,1720-1785`; `Pool/CasPool.h:1069-1085`).
- Test 12: `ManualBarrier` before the base `write` (`cas_test_helpers.h:128-159`).
- Test 13: the same barrier plus a real durable fence-out and `tryRemountOnce`; merely calling `tripMountLost` is insufficient to create runtime B.
- Test 14: implementable for the per-runtime cap, but it does not test the missing aggregate bound.

REVISE
