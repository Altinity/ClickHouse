# Codex review round 6 of spec rev.6 (gpt-5.6-sol, high) {#codex-review-round-6}

Reviewed commit: 6ac5a4bd9a7. Verdict: REVISE (all round-5 findings resolved; new findings are missing regression tests for the rev.6 branches). Findings verbatim:

## Round-5 status

- [MAJOR] Cache-hit admission revalidation — RESOLVED — §3.2 “The ledger API” rechecks `op.admitted()` inside the stripe.
- [MAJOR] Exception during `on_success` — RESOLVED — §3.2 resets both the set and byte count before propagating.
- [MAJOR] Deterministic tests 12–13 — RESOLVED — §4 adds a stripe-waiter observation.
- [MINOR] Test 14 weight observability — RESOLVED — §4 specifies a test-only weight accessor.
- [MINOR] Bounded-wait claim — RESOLVED — §3.2 “Cost of the stripe” now distinguishes bounded holders from unbounded queueing.
- [MINOR] Cache-budget documentation — RESOLVED — §6 includes namespace-name bytes.
- [MINOR] Post-LIST lookup ambiguity — RESOLVED — §3.2 requires a fresh exact-life lookup and pinned `shared_ptr`.

## Simpler alternatives

- [MINOR] §3.2 — Admit the `CasOperation` inside `listNamespaceFilesHeld`, after acquiring the stripe. `CasRefLedger` already owns `mount_requests`; this removes the move-only parameter and places admission directly at the hit linearization point.
- [MINOR] §4 test 14 — Use a broad-gap fixture: a fixed budget that retains two small runtimes, followed by a name set larger than the remaining margin. This avoids the new weight accessor.

## New findings

- [MAJOR] §3.2 / §4 — The exact lost-admission race remains untested. Test 9 loses the fence before the call; test 13 starts the listing after remount. Add: block a first LIST after its backend result while it holds the stripe, start a second LIST and observe it waiting, trip the fence without remounting, then release the first LIST. The second call must refuse rather than return the newly populated old-generation cache. This is implementable with `ManualBarrier`, a `CountingBackend::list` override, and the proposed waiter seam (`src/Disks/tests/cas_test_helpers.h:124-159,1720-1749`).

- [MAJOR] §3.2 / §4 — No test exercises an exception from `on_success` after the PUT is durable. Add a deterministic injected cache-mutation failure, assert that the public PUT throws, and assert that the next listing performs a LIST and sees the durable name. This protects the destructive consumers at `ContentAddressedTransaction.cpp:1164,1295`.

- [MINOR] §4 test 13 — “Whether it settles as success or refused” contradicts the existing request contract. Once the blocked backend write lands after the fence generation changed, the post-commit gate must refuse it (`Backend/CasRequests.h:237-240`). Require the writer to throw and the subsequent LIST to contain the landed name.

- [MINOR] §4 tests 12–14 — Both proposed ledger observers widen the compiled production surface: tests hold only `PoolPtr`, while `ref_ledger` is private (`Pool/CasPool.h:1245`), so forwarding `Pool::*ForTest` methods are required. The waiter counter also adds production-side accounting. Follow the existing test-seam block (`Pool/CasPool.h:937-1138`) and increment only on the contended `try_lock` path.

## Ambiguities and placeholders

- [MINOR] §3.2 “The ledger API” — `listNamespaceFilesHeld(life, op, ...)` does not state the parameter type. Passing `const CasOperation &` is sound: the call is synchronous, does not retain or share it, and uses only `generation` and `admitted`; passing by value would require `std::move` because the type is move-only and single-threaded (`Backend/CasRequests.h:237-258`).

- [MINOR] §4 test 14 — `ref_table_cache_bytes` cannot be set after reading the weights: configuration is fixed at `Pool::open` (`Pool/CasPool.h:321,747,1192`). Specify a close/reopen sequence with the measured budget, a new test-only setter, or use the broad-gap fixture.

- [MINOR] §6 — “its user-facing description” is unnamed; the Antalya branch has no documentation occurrence of `ref_table_cache_bytes` beyond `Pool/CasPool.h:311-321`. Name the target document or remove this placeholder.

The mechanism is implementable, but the two new correctness branches need deterministic regression tests before implementation planning.

REVISE
