---
description: 'Live backlog — ref protocol: rev.6 lease-boundary exclusivity, the ref-lane state machine, and ref-ledger internals.'
sidebar_label: 'Ref protocol'
sidebar_position: 1
slug: /superpowers/cas/backlog/ref-protocol
title: 'CAS Backlog — Ref protocol'
doc_type: 'guide'
---

# CAS Backlog — Ref protocol {#ref-protocol}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for the ref-lane/ref-ledger
protocol: rev.6 lease-boundary exclusivity, ref-log recovery, and the ref-lane state machine.

## Rev.6 lease-boundary exclusivity (highest-priority open design) {#ref-protocol-rev6}

- **[Late Predecessor PUT] cross-epoch late-materialization correctness limitation** — HARD — The hazard rev.6 closes: a fenced predecessor's in-flight PUT can materialize below successor snapshot coverage (a missed `−1`/`+1` = data-loss class). Phase-1 documents it; the fix LANDED as the v9 in-band `EpochSeal` (INV-2, Stage A). `CasRefLatePredecessorObserved` (B4) is deleted from the tree (a historical comment in `gtest_cas_ref_writer.cpp` remains); end-to-end LIST-liar fault injection = Stage A T13.
- **[MOUNT-CLAIM-EPOCH-REGRESSION] should `claimMount` permit epoch regression?** — QUESTION (surfaced by Stage A T12, 2026-07-29) — `claimMount` (`CasServerRoot.cpp` ~:395) reclaims a same-uuid body that is gc_fenced / clean-marked / proven-dead WITHOUT comparing epochs, so a fenced twin holding a HIGHER allocated epoch is legally reclaimable while the fresh writer proceeds with a LOWER `writer_epoch` — an epoch regression at the mount claim. Intersects the same-uuid recreation epoch-counter reset (quiesce = primary defence). Decide: must the claim gate require fresh `writer_epoch` above the reclaimed body's epoch (new `MountClaimResult` field), or is regression benign under the seal grammar? Sharp edge to verify: `prev_epoch_seal` is required iff `writer_epoch > life_epoch`, so a regressed writer may skip the seal obligation — confirm that path cannot readmit a Late-Predecessor window. T12 deliberately did NOT add a `chassert` here (it would abort a path the claim logic permits); surviving guards: unclean-reclaim classification, exhaustive `-Wswitch`, operator log line. (An orphaned 2026-08-04-triage finding — the general case of `claimMount` reclaiming a same-UUID fenced/dead body without epoch comparison — folds in here as the same question stated generally.)
- **[refsnaplog Phase 2] measured ref-log/snapshot optimizations** — DESIRABLE (measurements-gated) — inline zero-byte log keys; GC-side fallback compaction for never-mounted tables; indexed/chunked multi-object snapshots; lazy snapshot blocks + byte-bounded row cache; per-round ref index; streamed snapshot construction; adaptive thresholds; decoded-body reuse; chunked namespace removal. Plus a **cross-epoch fault-injection integration test** reproducing the late-predecessor counterexample. (An orphaned 2026-08-04-triage finding lists the same Phase-2 optimization candidates verbatim — folded in as confirmation.)
- **[timeout-retry RFC residuals] bounded lease-aware S3 timeout/retry controller** — PARTIAL — `CasRequestController` (single-attempt conditional writes, budget, fence-gating, exact-key resolution) landed for the ref lane. RFC residuals still open: (a) AWS SDK region-redirect retry can bypass `ShouldRetry` when a client is `aws-global` (CAS disks are not aws-global today — add a startup guard/probe if that changes); (b) `promoteStaged`'s `copyObjectConditional` (server-side conditional copy) is a separate conditional-write mechanism NOT bounded by the single-attempt work — verify its retry semantics before relying on write-once promote; (c) bounded read/HEAD/LIST retries + startup validation for the non-ref plain-object paths (`casPutObject`/`casRemoveObject` still use the disk's default retry policy). (An orphaned 2026-08-04-triage finding lists both residuals verbatim — folded in as confirmation.)

## Ref-ledger internals and lane state machine {#ref-protocol-ledger}

- **[ORPHANED-ADJUDICATION-COMMENT] `CasRefLedger.cpp:108-120` documents an adjudication its neighbouring code does not perform** — {#orphaned-adjudication-comment} — MINOR — A comment describes a `mine | successor's seal | foreign` adjudication with a narrow `catch`, which is NOT what the function beside it does — a comment that misdescribes its neighbour is worse than no comment, and this region is exactly where the next reader will look when INV-2's chain-link grammar is next touched. Take it with the next sweep that reaches the file; re-derive what the comment SHOULD say from the code rather than deleting it blind. Related: `chainLinkFor` stays in an anonymous namespace, so INV-2's grammar cannot be swept in isolation — asserted through `prepareRefChunk`'s validator instead (accepted disposition).
- **[DEAD-INSTALL-PROBE-AND-STALE-REGION-COUNT] the post-durable install seam has a stale region-count comment** — {#dead-install-probe-and-stale-region-count} — MINOR — The dead-test-hook half is fixed (`gtest_cas_ref_ckpt.cpp`'s carve-time fence now sets the probe). Still open: `CasRefLedger.cpp:1903` says "Post-durable install region 2 of 3" although the restatement deleted the third region, and the fence's own comment now says "BOTH" while this comment still says "2 of 3" — a self-contradiction.
- **[LANE-TERMINAL-REPORTED-AS-RETRYABLE] one arm sets `Faulted` and hands survivors the retry-later class** — {#lane-terminal-reported-as-retryable} — MINOR (one-line fix) — `commitRefChunk`'s "lane not `Ready` at new-id allocation" arm sets `RefLaneState::Faulted` but completes survivors with the retry-later exception class, contradicting the stated contract (`Faulted` should map to `CORRUPTED_DATA`). Self-limiting (one spurious retry, not a loop), but a contract worth stating is worth not contradicting in one arm.
- **[LANE-WITNESS-NAMES-MORE-THAN-IT-PROVES] a lane-battery witness proves less than its name, and one adoption arm has no witness at all** — {#lane-witness-names-more-than-it-proves} — MINOR — `saw_retry_created` witnesses that a retry created durability, not that the adoption install happened, but `CaRefLaneCore_RESULTS.md` calls it "retry-created adoption" — overstated. No witness asserts the `Wedged → Ready` durable-adoption arm at all. Fix: correct the RESULTS wording and add a witness on the adoption install itself. Does not affect the blocker-dissolved verdict.
- **[PART-WRITE-RELEASE-SEAM] the `PartWriteTxn`/`PreparedPartWrite`/receiver-guard ownership seam needs its own contract spec** — HARD — USER-DIRECTED extraction, 2026-07-29. The relink redesign's review rounds kept grinding on one seam: three layers each with their own abort/retry, an overloaded `isTerminal`, nine scattered proven-no-send exits erased into a generic `NETWORK_ERROR`, false ERROR/WARNING log lines on settled-late releases, and no exactly-once emission contract for unproven releases. Extracted into a standalone spec as relink-independent prerequisite plumbing (single-`attempted`-bit proof channel, destructor-owned last-word emission, severity ladder, marker-sync fix); lands before relink implementation.

## Ref-ledger follow-ups from the two-model adversarial consult (2026-07-21) {#ref-ledger-consult-followups-2026-07-21}

Consult-flagged, controller-verified, deliberately deferred with measurement/design gates. Evidence:
`docs/superpowers/reports/2026-07-21-reftablestate-experiments.md`, `tmp/consult-gpt56sol-answer.md`.

- **Post-durable-PUT allocation window in the ref-lane flush** — folded into the publish-confirm
  fetch-handoff work and tracked there, not here; this pointer stays only so the finding isn't
  rediscovered. Two round-2 nuances not to lose
  when restructuring: the catch's "permanently unreplayable" framing over-claims (the covered region
  can throw via `MemoryTracker` limits on a durable+applied transaction); wedge resolution followed
  by flush is a path `BM_FlushInstall` does not model yet — measure it before changing anything.
- **Recovery re-runs 3-4 codec passes per snapshot row** (measured, est. 2-3x recovery/GC-rebuild
  cut) — `recoverRefTableDetailed` decodes, `stateFromSnapshot` re-encodes+re-decodes (hand-built
  defense), then per-row size helpers re-encode again. Fix: a validated-witness type
  `decodeRefTableSnapshot` produces that `stateFromSnapshot` accepts without the round-trip.
- **`precommits` is a plain `std::set`, deep-copied per state scratch copy** — bounded only by the
  ~64 MiB admission byte budget, not the 1,000-op cap; every shipped "O(1) ~58 ns copy" benchmark
  used a one-precommit fixture. Do not build a third COW container without a number: extend
  `BM_ScratchCopy`/`BM_Admits` with a P-sweep (1/100/10,000) first.
- **GC per-table recovery gate before ref-log fold** (defense-in-depth; mandatory before any
  multi-writer or rolling-upgrade-skew milestone) — refuted as a live defect today (single
  lease-holder cannot mint the fabricated history this would catch), but still worth building before
  that changes. Fix shape: per-table `recoverRefTable(ns)` before folding new logs, `CORRUPTED_DATA`
  clamps the table (no cursor advance) rather than aborting the round. (An orphaned 2026-08-04-triage
  finding states this same gate verbatim — folded in as confirmation.)

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[recovery-seal-greatest-applied-gap] recovery can publish a seal without advancing `greatest_applied` to it** — DESIRABLE — Codex review finding: next epoch's first log could use a stale `prev`, letting GC accept a late void log below the cursor. No evidence this specific ordering gap was independently closed by the Stage A/B rework — distinct from the general `committed_through` ceiling proof (verified separately), which does not cover `greatest_applied` specifically.
- **[recovery-repair-buffer-unbounded] verify recovery-repair memory bound matches the streaming-replay design** — DESIRABLE — Recovery repair buffering allegedly retains a whole omitted transaction (up to 20 MiB) rather than one decoded transaction at a time. Re-check against HEAD's `runRecoveryWalkOnce`/streaming replay path; if the buffering is now bounded, close as done, else this is a real soak-risk on large pools.

## No query-cancellation checks in the CA tree (2031-triage CAS-015) {#no-query-cancellation-checks}

Every CA wait (ref-lane single flight, leader election, namespace recovery, part-folder single
flight) is bounded by the I/O underneath it — the `CasRequestController` budget (16 attempts / 90 s),
the 120 s `recovery_retry_budget_ms`, or a mount-fence loss — so the "hangs forever" framing is
wrong, and the shutdown drain is explicitly timed (`CasRefLedger.cpp:1877-1895`, `wait_until` on a
shared deadline, fail-closed). What is genuinely missing: not one wait in the CA tree polls query
cancellation, so `KILL QUERY` and `max_execution_time` cannot interrupt a query parked behind a slow
leader, and several bounded operations in sequence can still add up to minutes.

Fix: thread a cancellation callback (the standard `isCancelled`/`QueryStatus` poll used elsewhere in
the read path) into the waits that run on a query thread — read/single-flight first, ledger waits
after. Related, already listed here: `[timeout-retry RFC residuals]` item (c) —
`PartFolderAccess` single flight rides the disk's default S3 retry profile instead of the CA
controller.

## Part-folder single flight keyed by ref only, no post-wait manifest check (2031-triage CAS-019) {#part-folder-single-flight-manifest-keying}

`PartFolderAccess`'s single-flight key is `ns+ref` (`Parts/PartFolderAccess.h:34`, `:383-384`) and a
waiter never re-checks that the view it receives is for the manifest id it resolved
(`.cpp:277,283,286-287`). Not the mixed-manifest hazard the audit claims: single flight covers only
the stale-tolerant `CachedForLoad` mode (`.cpp:267-270`), every view is an internally consistent
single-manifest snapshot, and all read-after-write paths use `ForceFresh`. The real consequence is a
one-repoint skew — a follower straddling a repoint can get the neighbouring manifest's view and
surface a spurious `FILE_DOESNT_EXIST`.

Fix: either key the single flight by `ns+ref+manifest_id`, or add the cheap post-wait check (compare
the served view's manifest id against the resolved one and re-resolve on mismatch). The second is
smaller and keeps the sharing benefit. P2.

## Allocation in `noexcept`/destructor paths under a memory limit (2031-triage CAS-018) {#noexcept-allocation-hardening}

The audit's headline (leadership leaked out of the ref queue on a throw) is closed: release is a
single unconditional authority (`Pool/CasRefLedger.cpp:2019,2022-2029,2039,2099`) and the historic
stranded-item bug was fixed in `79c07d6cc3d` with regression tests in
`gtest_cas_ref_lane_exception_safety.cpp`; its "renewal fences the mount" sub-claim is simply false
(the hook is wrapped, `Pool/CasServerRoot.cpp:1474-1483`). What remains is hardening nits: string
formatting / container work inside `noexcept` functions and destructors that can throw under a
memory limit — `Gc/CasGcPhaseTimer.h:52-75`, the `Backend/CasProbe.cpp` cleanup lambdas,
`Pool/CasMountRuntime.cpp:529-532`, and the fail-closed branch at `CasRefLedger.cpp:2085-2090`.
P3: wrap or pre-size those, no behaviour change intended.
