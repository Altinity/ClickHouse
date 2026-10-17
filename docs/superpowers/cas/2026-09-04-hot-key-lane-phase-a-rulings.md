---
description: 'Record of the decisions taken while implementing phase A of the CAS hot-key write lane: the controller rulings made on the owner''s behalf, the final whole-branch review verdict with its checklist verification, and the codex end-to-end review findings with how each was folded.'
sidebar_label: 'Hot-key lane phase A rulings'
sidebar_position: 46
slug: /superpowers/cas/hot-key-lane-phase-a-rulings
title: 'Hot-key lane phase A: rulings and review record'
doc_type: 'reference'
---

# Hot-key lane phase A: rulings and review record {#hot-key-lane-phase-a-rulings}

Branch `cas-hot-key-lane`, 2026-09-04, implementing `docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md`
(revision 34) through `docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md`. Thirteen commits over
`e59fe7e8e4b`; the `CAS*` gate 2406/2406 on the final tree. Deferred items: `BACKLOG.md` `{#hot-key-lane-phase-a-followups}`.

## Rulings made during execution {#rulings}

Decisions the controller took where the plan, the spec or a review left a question, in the order made. Each says
what it costs if wrong.

- Ruling: T6's merge into `cas-gc-rebuild` is performed only after the user confirms — a merge into a shared branch is one of the four stop conditions — what it costs if wrong: nothing, the branch waits.
- Task 2: implementer BLOCKED on two brief test defects (no commit yet). Ruling: ResultsAreTheEnginesOwn sub-cases 4 and 5 arm the read failure from a one-shot onBeforeWrite hook so the hold's own base read is not what consumes it; expectations unchanged (Conflict{NotObserved}, GaveUp{Unresolved}) — cost if wrong: one test asserts a path the lane cannot reach, caught by the reviewer.
- Task 2: Ruling: WaitersLeave... sets lease_spent = true before spawning the lease waiter and drops the depth poll, keeping alive false, so both refusals are present at its first slice and the engine's order (lease before liveness) is what the assertion pins — cost if wrong: the test proves the order for a stale flag rather than a simultaneous flip; acceptable.
- Task 2: Ruling: `friend class CasHotKeys` on CasRequests as well as CasOperation (friendship is not transitive through `owner`), and CacheBase's six-argument constructor with NO_MAX_COUNT/DEFAULT_SIZE_RATIO as CasManifestReader does — both within the spec's "friend for the engine's privates" change; no new engine surface — cost if wrong: none.
- Task 3: Ruling (pre-dispatch): in TheCacheForgetsWhatItCannotVouchFor the "Unresolved after a send" sub-case cannot combine a cached start with `Retry::once` (once bypasses the cache, so the up-front armed read failure would be consumed by the base read, the same defect Task 2 hit); the sub-case arms the ambiguity and the read failure from a one-shot onBeforeWrite hook under `Retry::once`, and expects reads before + 3 (base read, failed resolve read, the next hold's read) — cost if wrong: one assertion count, caught by the reviewer.
- Ruling (user directive 16:58): pipeline reviews with the next task's implementation (reviewer reads the diff package; one implementer at a time in lane-g); fix rounds queue behind the running build — cost if wrong: a fix round waits for one build.
- Ruling (user directive 17:33): the master worktree is not mine — no reports on it, no commits or merges there; Task 6's merge step is replaced by 'branch green and reviewed, ask the user'; the SDD workspace (git-ignored scratch) stays where it is — cost if wrong: none.
- Task 4: Ruling: those two tests violate the lane's stated rule (no callback that runs while a ticket is held submits to the lane) and model another server; fix them by giving the rival its own CasRequests (own private lane), as PoolAndExternal does; done as fix round 1 within Task 4 — cost if wrong: a test edit outside the brief's file list, reviewed.
- Task 4: Ruling: the plan-mandated pacing-test weakness is fixed (K = 8 in the settled-fault scenario, assert at least one sleep > 200 ms) — cost if wrong: none, the assertion is strictly stronger.
- Task 5: Ruling: the pool test's external-writer cell counted every backend GET, but the ledger's own catalog snapshot reads (CasRefCatalog::read, outside the lane by design) make that 4 per fresh namespace; the assertion uses the lane's own signals instead: CASRequestResolveRead delta == 1 (the one resolve read the stale hint costs), CASHotKeyReadStarts delta == 0 (the next hold starts from seen), writes delta == 3 unchanged — cost if wrong: one test measures the lane's counters rather than raw GETs; the spec's test 12 already says the pre-check reads are not counted.
- Task 6: Ruling: finding C is load-bearing (a supported non-S3 build would be red) and the repair is one conditional constant, so one more scoped round is dispatched to the same fixer with the two cheap out-of-scope items folded (config.h include; chassert on the item find); after it, a scoped re-review, then stop and ask the user — cost if wrong: one extra small round.

## Final whole-branch review (opus) {#final-review}

## Verdict

**Mergeable after the listed fixes.** One Important finding: a new test aborts the whole unit-test
binary in debug and sanitizer builds, so the `CAS*` gate is not green on those CI lanes. No Critical.
The production code is sound: I walked the lane end to end from `casUpdateImpl` through `submit`,
`hold`, the engine's `writeLoop` and back, and found no path on which a conditional write can land
past the store's precondition, on which a verdict rendered on a cached base reaches a caller, or on
which a lock cycle or a use-after-free is reachable from production.

## Checklist verification

The rules revision 34's history names as "the checklist a review verifies against", each against the
code rather than against the reports.

| rule | where it is kept |
|---|---|
| A verdict on a cached base is never delivered, whatever refused its validation | `CasHotKeys::hold`: `verdict_on_hint` is set both by a decline (`from_cache && !candidate`) and by the `catch (...)` arm; it drops the entry, re-reads, and returns the read's own give-up when the read is refused. **Kept.** |
| The base read is the engine's `observe` with `readModifyWrite`'s conversion, never the throwing `read` | `hold`'s `read_base` lambda: `op.observe` then `op.gaveUpAfterFailedObservation`. The throwing `op.read` appears in the lane nowhere. **Kept.** |
| The caller's policy is frozen once, not a fresh `standard` | `casUpdateImpl` takes `op.freeze(policy)` before the loop and passes it to every submission; `CasHotKeys::submit` calls `op.freeze` again, which returns an already-frozen policy unchanged. `create` and `replace` re-bind, but a frozen policy binds to the same absolute deadline. **Kept.** |
| `Conflict::any_ambiguous`, because `attempts_sent` cannot stand in | The field is on `Conflict`, set at all three return sites in `writeLoop` and `readModifyWriteOnPresence`, cleared per inner write at `writeLoop`'s entry. `ReplaceReportsWhetherAConflictSettledAFault` pins both values at `attempts_sent == 1`. **Kept.** |
| A cache fill that throws after a landed `PUT` is never the result | `CasHotKeys::remember` is `noexcept` and wraps the weight computation and `CacheBase::set` in a try that logs, drops the entry and returns. `TheCacheForgetsWhatItCannotVouchFor` arms `cache_fill_hook_for_test` after a landed write and asserts `Committed`. **Kept.** |
| The guard is the single remover and neither allocates nor throws | The `Leave` struct in `submit` is the only caller of `leave`, which is `noexcept`; under the mutex it does a deque erase, an optional assignment and a map erase, none of which allocate. The profile event and the log line are outside the lock and inside a catch-all. **Kept**, with the caveat that `std::lock_guard` construction itself is outside the try (minor 6). |
| A waiter re-checks its own fence, `Liveness`, lease and deadline every slice, in that order | `submit`'s loop runs `op.gate(0)` (fence generation, then lease budget, then liveness, reported as `FenceLost`) and then `op.fits(0, bound)`, both outside the mutex, before every front check and after every `cv.wait_for` slice. `WaitersLeaveOnTheirOwnFenceLeaseAndDeadline` pins the order by making the lease and liveness refuse at the same slice and asserting `Deadline`/`Lease`. **Kept.** |
| The lane is erased only when empty | `leave`: `if (lane.queue.empty()) lanes.erase(it); else cv.notify_all();`. The only other map mutation is `try_emplace` and its rollback in `submit`. **Kept.** |
| No attempt cap on the catalog loop | `casUpdateImpl`'s `for (;;)` has no counter; `kMaxCatalogCasAttempts` remains used only by the GC erase loop. Termination comes from `submit` returning `GaveUp{Deadline}` once the frozen bound passes. **Kept.** |
| The 200 ms pause overshoot accepted | `casUpdateImpl` uses the bare `op.pause`, with no gate or `fits` check around it. **Kept.** |
| Eviction of a stalled holder rejected | No eviction path exists; `hold` runs to completion on the caller's thread and only its own guard removes its item. `AThrottledHolderKeepsTheWaitersQueuedThroughItsBackoff` asserts the waiters land in arrival order after the holder's reissue. **Kept.** |
| The `driver_mutex` audit | `MountConfig`'s three hooks that run under `driver_mutex` now carry the contract at their declarations. `CasMountRuntime.cpp` is unchanged, and I re-verified the load-bearing row: `remountLoop` closes its lock scope before calling `remount_attempt`. **Kept.** |
| The step-1 marker catch | `createNamespace` catches `CatalogFenceMovedMarker` around `createNamespaceStep1` only and returns `FencedOut`; `casAdmitEntry` catches it and calls the `[[noreturn]]` `throwCasTransientUnavailable`. `AFenceLostDuringStepOneIsFencedOutNotABareMarker` covers all three arms. **Kept.** |
| Engine changes are exactly the four the spec names | `CasRequests` carries the pointer and `CasOperation::hotKeys` exposes it; `friend class CasHotKeys` on `CasOperation`; `Conflict::any_ambiguous`; `Retry::conflictBackoff` with `pauseForConflict`. A fifth grant, `friend class CasHotKeys` on `CasRequests`, was added because friendship does not reach through `CasOperation::owner` to the clock and sleep the lane reads; it is recorded as a ruling in the ledger and adds no public surface. **Kept, with the recorded exception.** |
| One instance per pool, declared before its three planes | `CasPool.h`: `hot_keys` sits between `meta` and `mount_requests`, so it is constructed first and destroyed last; all three planes are constructed with `&hot_keys`. The two bootstrap `CasRequests` in `CasPool.cpp` and the two in `Tools/CasDecommission.cpp` get private budget-0 lanes. **Kept.** |
| No durable-format, key-shape or protocol-step change | The three catalog-key writers are unchanged in shape: the bootstrap `create`, `casUpdateImpl`'s one conditional write per submission, and the GC erase's `replace`. Nothing persisted changes. **Kept.** |
| Comments state the reason and cite no plan, spec, review or task | A sweep of the added lines for `BACKLOG`, `spec`, `§`, `Task N`, review rounds, finding ids and `INV-N` returns nothing. **Kept.** |
| Allman braces | No added line opens a brace on the same line as an `if`, `for`, `while` or `switch`. **Kept.** |
| Contract edits: three sentences in the backend token contract | All three landed: the `readModifyWrite` conflict-pace paragraph, the hand-written-loop rule and its site-table row, and `any_ambiguous` in the result vocabulary. **Kept.** |

## End-to-end review (codex gpt-5.6-sol, high) {#codex-review}

Findings on the branch before the fix wave; each MAJOR was folded (commits `bc57cdcdb5e`, `9ac8a45c405`,
`2f1c51aa450`) and verified by two scoped re-reviews. The minors are in the BACKLOG item.

1. [MAJOR] The `noexcept` ticket guard can terminate before removing its item  
   `CasHotKeys.cpp:146-148` calls the potentially throwing `now_ms` callback before acquiring `mutex`; the default `clock_gettime_ns` itself throws on clock failure.  
   If this happens during normal return or unwinding, `Leave::~Leave` invokes `std::terminate`, and the guard never performs its sole-remover duty. Remove the ticket first; perform timing and logging afterward inside a catch-all.

2. [MAJOR] A failed cache fill can corrupt LRU accounting  
   `CasHotKeys.cpp:47,292,297` computes weight twice, and `Etag::render` allocates. If the first call succeeds but the call from `LRUCachePolicy::set` fails while replacing an entry, `LRUCachePolicy.h:181-187` has already subtracted its old size; `remember` then calls `forget`, subtracting it again.  
   This can underflow internal size accounting, invalidate cache metrics, and eventually defeat the byte bound. Precompute and store a numeric weight before entering `CacheBase::set`, making the weight functor non-throwing.

3. [MAJOR] The new test file does not compile without AWS S3  
   `gtest_cas_hot_keys.cpp:618` uses `s3Error` and `Aws::S3::S3Errors` unconditionally, while both are available only under the `USE_AWS_S3` guard at lines 97-102.  
   A supported `ENABLE_AWS_S3=0` build therefore fails compiling `unit_tests_dbms`. Guard this refusal subcase or provide a backend-neutral definite-refusal fixture.

4. [MINOR] `ResultsAreTheEnginesOwn` does not verify the engine’s result contents  
   `gtest_cas_hot_keys.cpp:196-279` mostly checks only the returned variant and selected fields; altered `attempts_sent`, `deadline_source`, `last_seen`, refusal code/message, or commit metadata can pass. The base-read comparison at lines 299-308 also omits `attempts_sent`.  
   Assert every field relevant to INV-4, preferably against the corresponding direct engine operation.

5. [MINOR] The GC-erasure test measures after the resolving submission already finished  
   In `gtest_cas_ref_catalog.cpp:2721-2753`, the parked holder is released and joined before counters are sampled; that holder is the first lane submission after the winning erase and already pays the conflict resolution. The later operation is checked only with `EXPECT_LE`, despite the comment claiming exact counts.  
   Measure across the holder’s release/join with exact counts, then assert the following submission performs zero reads and one write.

6. [MINOR] The engine test does not prove settled faults use growing backoff  
   `gtest_cas_requests.cpp:1499-1519` checks only upper bounds. An implementation that incorrectly sleeps within flat `[0,200]` for every fault would satisfy every assertion while still incrementing `CASRequestReissue`.  
   Add a deterministic jitter seam or otherwise make the test distinguish later growing ceilings from `conflictBackoff`, covering INV-6 without probabilistic inference.

The increment is not safe to merge yet. The ticket guard must be made unconditionally cleanup-safe first, followed by making cache weighting non-throwing and restoring the no-S3 build; the remaining test gaps should then be tightened so regressions in the lane’s result and pacing contracts cannot pass unnoticed.
