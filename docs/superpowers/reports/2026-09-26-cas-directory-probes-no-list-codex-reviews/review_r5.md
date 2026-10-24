# Codex review round 5 of spec rev.5 (gpt-5.6-sol, high) {#codex-review-round-5}

Reviewed commit: 9c027cd341e. Verdict: REVISE (no CRITICAL; mechanism accepted, findings are implementation tightenings). Findings verbatim:

## Round-4 status

- [MAJOR] Missing post-settle ledger operation — **RESOLVED** — §3.2 “What serializes them” and “The ledger API” replace the two-runtime protocol with a life-keyed stripe that survives runtime replacement.
- [MAJOR] Aggregate cache budget — **RESOLVED** — §3.2 “Memory” adds `namespace_file_names_bytes` to `weightOf` and enforces `ref_table_cache_bytes` after growth.
- [MAJOR] Per-table cap contradicting the warm guarantee — **RESOLVED** — §3.2 “Memory” removes the cap; names remain cached exactly while their runtime remains resident.
- [MINOR] Ambiguous DELETE test fault — **RESOLVED** — §4 test 8 explicitly keeps throwing after later `remove` attempts returning `Gone`.

## Simpler alternatives

None found. A ledger-wide mutex is operationally worse, while per-life dynamically allocated mutexes require their own lifetime management. The fixed striped bank is the smallest practical design.

## New findings

- [MAJOR] §3.2 “Read path” — A cache hit is not revalidated after acquiring the stripe. `Pool::listNamespaceFiles` checks `op.admitted()` before calling `listNamespaceFilesHeld`, but passes only its generation. The fence can be lost while the caller waits for the stripe; the old runtime remains attached until remount, its generation still matches, and the populated set is returned. `CasOperation::admitted` is explicitly the dynamic verdict point (`Backend/CasRequests.h:237-258`). Pass an admission callback or the operation and check it inside the stripe at the cache-hit linearization point. §4 test 9 covers loss before the call, not this race.

- [MAJOR] §3.2 “The ledger API” / “Write path” — Cache-update exceptions can leave a settled PUT missing from the populated set. After `write_fn` succeeds, `on_success` performs a potentially allocating `std::set::insert`; the specified reset applies only when `write_fn` throws. A memory-limit/allocation exception therefore propagates after the object is durable while preserving a stale set, which destructive enumeration can later trust (`ContentAddressedTransaction.cpp:1164-1166,1293-1303`). Any exception while applying the successful cache mutation must reset the set and byte count before propagating.

- [MAJOR] §4 tests 12–13 — The intended write-first interleavings are not deterministic with the stated helpers. `ManualBarrier` can prove the write is parked in `CountingBackend::write`, but there is no observation that the LIST caller has reached and blocked on the stripe; a buggy implementation can pass if that thread is scheduled only after the write is released. `CountingBackend::list` cannot provide the signal because the correct implementation never reaches it before release (`src/Disks/tests/cas_test_helpers.h:124-159,1720-1785`). Add a ledger test seam/waiter observation or specify another deterministic schedule.

- [MINOR] §4 test 14 — Implementable, but “just above one runtime’s ref weight” is not directly observable: existing test APIs expose residency, not weight (`Pool/CasRefLedger.h:523-532`). Specify either a test-only weight accessor or a broad-gap fixture that asserts both runtimes are resident before installing a pre-seeded name set. The eviction counter and installed-runtime checks are otherwise implementable.

- [MINOR] §3.2 “Cost of the stripe” — “A wait is bounded by one request’s retry policy” is false for `std::mutex`. A waiter may queue behind multiple colliding operations or starve under continued arrivals. Only each holder’s request duration is policy-bounded.

- [MINOR] §6 — The documentation scope omits the cache-budget contract. `Pool/CasPool.h:311-321` currently defines weight as snapshot bytes plus retained log-tail bytes; it must also document namespace-name bytes.

## Ambiguities

- [MINOR] §3.2 “The ledger API” — “When such a runtime exists” should explicitly require a fresh post-LIST lookup, exact `NamespaceLifeId` equality, and a pinned `shared_ptr`. Reusing the pre-LIST pointer can install only into a detached runtime. This is wasted caching rather than a stale-successor schedule, because the successor’s LIST still needs the same stripe.

No additional stale-set interleaving or lock cycle was found across eviction, remount, recovery, `ref_queue_mutex`, `state_mutex`, or the hot-key lane, assuming exact-life lookup and `shared_ptr` pinning. Updating the current runtime irrespective of generation is correct: same-life successors share the stripe, while reborn lives use different physical prefixes.

REVISE
