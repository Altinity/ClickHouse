# RCA: flaky `CASSentinelProbe.NativeClassifiesUnmodeledErrorAsIndeterminate`

**Verdict: product defect (boundary off-by-one in `probeSentinel`), not ASan- or order-specific. Pre-existing on the base.**

## Mechanism
- `CasRequests.cpp:701-709`: after an `Indeterminate` probe, the loop draws `pause_ms = Retry::backoff(attempt)` (full jitter, `CasRetry.cpp:11-18`, uniform [0, 5000]) and tests `fits(reservedFor(pause_ms, 1), bound)`. If it does not fit it RETURNS the `Indeterminate` result (the intended end).
- `fits` (`CasRequests.cpp:445-452`) is non-strict on the sum (`needed <= deadline - now`) but strict on `now < deadline`.
- The test has `attempt_reservation_ms == 0` (`ObjectStorageBackend` built with attempt/connect caps 0, `CasObjectStorageBackend.h:112-115`; `CasRequestBudget.cpp:18-26`). When the last draw equals the remaining time exactly, `needed == deadline - now`: the bottom check passes, `FakeClock` sleeps the pause, `now == deadline`.
- Next iteration, the top check `fits(reservedFor(0,1), bound)` (`CasRequests.cpp:672`) fails on `now >= deadline` and calls `giveUpReadDeadline("probeSentinel", ..., attempt - 1)` (`:673`, defined `:495-501`), which throws `CAS write could not be committed ... gave up at the policy deadline after N attempt(s)`. The N in the gate log (40) is the attempt count at that moment.
- With a non-zero reservation (production) the two checks are equivalent (`now' + R <= deadline` both times), so the throw needs R == 0 plus an exact-equality draw. In production the same off-by-one exists only for a backend with zero attempt envelope; there the contract "returns Indeterminate after a probe ran" is violated.
- Not involved: the real clock (test injects `FakeClock`, `cas_test_helpers.h:259-279`), shared/static state, test order. The only nondeterminism is `thread_local_rng` in `Retry::backoff`; ASan is incidental.

## Measured rate (build_asan/src/unit_tests_dbms, single process)
- Test alone, `--gtest_repeat=300`: 1 failure (attempt count 35 in the message).
- Test alone, `--gtest_repeat=10000`: 6 failures (6e-4), attempt counts 38-47.
- Whole `CASSentinelProbe.*`, repeat=50: 0 failures; with `--gtest_shuffle`, repeat=50: 0 failures (each has 50 runs of the test; expected ~0.03 failures).
- Model: each run passes 1-3 steps with remaining time <= 5000 ms; each has P(pause == remaining) = 1/5001. That gives ~4-6e-4, matching. Combined with ~2600 tests per gate this is a ~0.05% per-gate flake; it has been rare only because of that.

## Touched by the branch or merge?
`git diff 61d24118251..ac48289f453 --stat` and `61d24118251..e2dd0f5d343 --stat` over `gtest_cas_sentinel_probe.cpp`, `CasRequests.cpp`, `CasRetry.cpp`, `CasRetry.h`: empty for both. The loop was last changed by 9a6bcb68aca (envelope reservation); the gap predates the branch. Pre-existing base flake.

## Fix plan
1. Make the reissue check in `probeSentinel` (and `removeCurrent`, `readLoop` at `CasRequests.h:512-513`, same shape) agree with the top check: require `now + pause < deadline` (strict) when deciding to sleep, or make the top check non-throwing after a probe has run (return the last `Indeterminate`). Smallest: in `fits`, use `needed_ms < deadline - now` only for the reissue call site, or compare `pause + 1`.
2. Add a deterministic unit test: `FakeClock` plus a way to fix the jitter (or a policy with `window_ms` equal to the drawn pause) so the last pause lands exactly on the deadline; expect `Indeterminate`.

## Not established
- Deterministic reproduction was not built (no edits or builds allowed); the equality mechanism is inferred from code plus the measured rate, not observed under a debugger.
- Whether `removeCurrent`/`readLoop` share the exact defect was not tested (they throw on give-up by contract, so the impact there is only the reported attempt count).
