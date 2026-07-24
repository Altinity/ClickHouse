# Codex adversarial reviews of CAS writepath stage 1 (2026-07-24) {#codex-stage1-reviews}

Two read-only reviews by codex `gpt-5.6-sol` (reasoning effort xhigh), launched via nohup during the
unattended stage-1 round. Round 1 covered tasks T1-T10 mid-stage; round 2 covered the complete stage
T1-T14 after closure. Verbatim results below; adjudications and fixes are recorded in the session ledger
and in commits `4829435a157`, `93a0f32e669` (round-1 fixes) and the codex2 fix round (round-2 fixes).

## Round 1 (T1-T10) — verdict: SOUND WITH FIXES {#round-1}

## Findings

### Critical — Fan-out can lose ownership of an already-runnable task

The fan-out submits through `enqueueAndKeepTrack` at [ContentAddressedTransaction.cpp:1694](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1694). That helper schedules the callback first at [threadPoolCallbackRunner.h:230](/home/mfilimonov/workspace/ClickHouse/master/src/Common/threadPoolCallbackRunner.h:230), then appends its task handle to the tracking vector at [threadPoolCallbackRunner.h:245](/home/mfilimonov/workspace/ClickHouse/master/src/Common/threadPoolCallbackRunner.h:245). The vector has not been reserved.

Concrete failure scenario:

1. A query near its memory limit starts a fan-out; the pool successfully accepts a task.
2. The subsequent tracking-vector allocation throws.
3. The task is absent from the runner’s tracked list, so its destructor cannot cancel or join it.
4. `fanOutBlobUploads` unwinds and destroys `results`, whose slots are captured by raw pointer at [ContentAddressedTransaction.cpp:1689](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1689). Transaction unwinding can also remove the local staging source at [ContentAddressedTransaction.cpp:174](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:174).
5. The queued task later dereferences the stale `txn` and `slot` pointers at [ContentAddressedTransaction.cpp:1698](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1698). The runner wrapper itself also captured the destroyed runner through `this` at [threadPoolCallbackRunner.h:205](/home/mfilimonov/workspace/ClickHouse/master/src/Common/threadPoolCallbackRunner.h:205).

The outcome is a use-after-free, possible memory corruption, and a race with deletion of the upload source. This directly violates the authoritative “destructor drains first on every path” lifetime contract.

Fix by allocating tracking capacity for all grouped tasks before the first submission, or by changing the runner API so a task is registered in preallocated storage before it becomes runnable. Add fault injection specifically between successful pool scheduling and tracking publication.

### Important — An allocation exception can permanently strand the ref-lane leadership baton

`appendRefOps` publishes `leader_active = true` and unlocks the queue mutex at [CasRefLedger.cpp:992](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:992). It then performs the first allocation for `owned_items` at [CasRefLedger.cpp:1005](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1005), before entering the catch at [CasRefLedger.cpp:1007](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1007).

Concrete failure scenario:

1. An item is queued on an idle namespace and elects itself leader.
2. `leader_active` becomes true and the mutex is released.
3. `owned_items.push_back(item)` throws during allocation.
4. Control exits `appendRefOps` without reaching `completeOwnedItemsAndReleaseLeadership`; its only normal reset is at [CasRefLedger.cpp:1084](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1084).
5. The original item remains pending. Every later caller observes an active leader and waits at [CasRefLedger.cpp:1029](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1029), although no leader exists. Shutdown/remount draining can only time out against the permanently non-idle state at [CasRefLedger.cpp:898](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:898).

This creates a permanent write outage for that namespace and violates the two-phase carve/no-waiter-hang contract before the carve even starts.

Prepare the leader’s initial responsibility storage before publishing `leader_active`, with no throwing operation between baton publication and installation of the exit guard. Add allocation-failure coverage at this exact pre-tenure point.

### Minor — `DispatchThrowStillDrains` is not synchronized with task entry

The test throws on the second `on_dispatch` call without waiting for the first worker to enter its task at [gtest_cas_upload_fanout.cpp:764](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_upload_fanout.cpp:764), but then requires the first body to exist at [gtest_cas_upload_fanout.cpp:782](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_upload_fanout.cpp:782).

On unwind, the runner marks tasks that are still `SCHEDULED` as cancelled at [threadPoolCallbackRunner.h:116](/home/mfilimonov/workspace/ClickHouse/master/src/Common/threadPoolCallbackRunner.h:116), and explicitly skips waiting for cancelled tasks at [threadPoolCallbackRunner.h:250](/home/mfilimonov/workspace/ClickHouse/master/src/Common/threadPoolCallbackRunner.h:250). Therefore a sufficiently fast caller can cancel the first task before its body runs, making the assertion flaky.

Use a latch from the first task’s `in_task` hook and have the second dispatch wait for that latch before throwing. That tests the design’s actual requirement: already-running work is drained before unwind.

### Minor — Comments falsely claim Task 5 parallelizes part publication

The declaration says its result slot is “load-bearing once Task 5 calls this from multiple threads” at [ContentAddressedTransaction.h:227](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h:227). The implementation similarly claims Task 5 dispatches `publishStaging` concurrently and makes publication order racy at [ContentAddressedTransaction.cpp:480](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:480).

In the final state, parts are still published by a serial loop at [ContentAddressedTransaction.cpp:495](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:495); Task 5 parallelizes only the blobs within each part. These comments overclaim current thread safety and confuse stage-1 with future cross-part work. Rewrite them as serial rollback bookkeeping or clearly label concurrency as future scope.

## Cross-task composition result

| Area | Result |
|---|---|
| Fan-out lifetime and ThreadGroup propagation | ThreadGroup propagation is correct, but task tracking has the Critical ownership gap above. |
| Dispatcher, semaphore, and lock order | Sound. The dispatcher releases its mutex before invoking the sink at [CasEventDispatcher.cpp:36](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasEventDispatcher.cpp:36). Condemned-local admission covers only materialization and `putOverwrite`, ending before event/meta work at [CasPartWriteTxn.cpp:729](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:729). No Pool/ledger/dispatcher/semaphore lock cycle was found. |
| Two-phase carve and chunk tenure | After tenure ownership is established, chunk survivor completion, earlier-chunk success containment, reseeding, and baton release are consistent. The pre-guard allocation gap remains a blocker. |
| Admission/encode/decode budgets | Sound. Admission and the codec use the same op-based removal discriminator at [CasRefLedger.cpp:1383](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1383) and [CasRefLogFormat.cpp:57](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp:57). The production `RemoveNamespace` builder is `WholeShard`-scoped at [CasRefLedger.cpp:2402](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2402), so it remains singleton across chunk boundaries. |
| Upload-overlap objective | Achieved once the tracking defect is fixed. Backend upload work runs concurrently; the remaining serialization is the intentional short event-sink funnel and the condemned-memory cap. Cross-part publication remains serial, as expected for stage 1. |

I did not flag retained payload fields or full-buffer recovery: those are explicitly T12/T13. Per the review rules, I made no changes and ran no builds or tests.

**VERDICT: SOUND WITH FIXES (block on the fan-out task-tracking UAF and pre-guard ref-lane baton leak; also fix the flaky dispatch test and stale concurrency comments).**
## Round 2 (T1-T14, full stage) — verdict: SOUND WITH FIXES {#round-2}

No remaining Critical production defect was found. The two earlier defects are fixed correctly. I found one Important validation gap and three Minor contract/truthfulness issues.

## Findings

1. **Important — T13’s “hard memory bound” test does not measure memory and cannot detect whole-tail materialization**

`PeakTracker` explicitly accounts only values supplied through the probe, not RSS or allocations (`src/Disks/tests/gtest_cas_recovery_streaming.cpp:31-35`). The probe is entered only from `RefReplayBuilder::applyOne`, after the object has already been fetched, decompressed, and decoded, and it receives the compressed/stored byte count rather than the decoded transaction’s resident size (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:482-490`). The test then defines its bound from those same stored sizes (`src/Disks/tests/gtest_cas_recovery_streaming.cpp:217-246`).

Concrete failure scenario: recovery lists a long tail of near-20-MiB decoded transactions; a regression fetches and decodes every transaction into a vector, then invokes `applyOne` sequentially. The complete tail is resident and may exhaust memory, but the probe sees only one stored-byte weight at a time, so `LongTailReplaysUnderMemoryBound` remains green. Highly compressible transactions make the discrepancy larger. This fails the plan’s explicit requirement for an RSS/allocation bound with a RED control that the old implementation exceeds (`docs/superpowers/plans/2026-07-23-cas-writepath-stage1.md:330-331`).

The current three production loops do stream visibly, so this is a validation/regression-guard defect rather than evidence that the current implementation materializes the tail. Replace the probe with tracked allocations or RSS across the full GET → decompress → decode → apply lifetime, and include a deliberately materializing control implementation that fails the bound.

2. **Minor — a newly published recovery seal leaves `RecoveryResult::sealed_from` stale, and the claimed complete inventory does not cover it**

The builder initializes `RecoveryResult::sealed_from` from the selected base snapshot (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:470-478`). During unclean recovery, the ledger constructs a new seal with a new `sealed_from` value (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:496-502`), but after publishing it updates only `newest_snapshot_id`, tail accounting, and base bytes (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:534-540`). Consequently, the result can identify the new seal while retaining the predecessor seal’s `sealed_from` or `nullopt`.

Moreover, `installRecoveryResult` claims to copy every field but has no destination or copy for `sealed_from` (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:620-635`), even though the struct includes it (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h:362-394`). The inventory test deliberately uses a clean recovery with no new seal and never asserts this field (`src/Disks/tests/gtest_cas_recovery_streaming.cpp:416-420`, `src/Disks/tests/gtest_cas_recovery_streaming.cpp:466-497`), contrary to the as-built claim that every current field is covered (`docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md:312-318`).

There is no current data-plane failure because the ledger runtime does not consume `sealed_from`; orphan sweep obtains it later by decoding the durable seal. Still, the “complete `RecoveryResult`” invariant is false. Set `result.sealed_from = seal.sealed_from`, then either give it an explicit runtime destination or narrow the installation contract, and add an unclean-seal inventory case.

3. **Minor — the removed snapshot payload field is silently accepted despite the no-tolerance policy**

The exact old ref-log wire word `set_payload` is correctly rejected because only `set_published_at` is recognized (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp:35-41`). Same-build log encode → decode → apply is consistent.

Snapshots differ: committed-row decoding remains tolerant and skips every unknown key (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp:265-296`). Therefore an old committed row containing `"pl"` is accepted, and its payload is silently discarded before the row is installed (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp:301-310`). The diff confirms `"pl"` was precisely the removed field (`tmp/codex_stage1_full.diff:1239-1240`, `tmp/codex_stage1_full.diff:1262-1272`).

The generic ref-op reader would likewise ignore `"pl"` if paired with the new op word (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp:183-202`).

Under the stated no-old-pools assumption this has no operational effect, and current-build self-consistency holds. It nevertheless contradicts “No decoder tolerance for the old field” (`docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md:269-271`). Explicitly reject `"pl"` in snapshot and op records, with negative decoding pins.

4. **Minor — the T14 truthfulness sweep still describes a nonexistent 64-MiB blob-object cap**

The server setting says 64 MiB is “the CAS object size cap” and equates the derived capacity to one full-sized condemned blob per worker (`src/Core/ServerSettings.cpp:156-164`). The pool header and implementation repeat that claim (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.h:126-131`, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.cpp:183-196`).

In reality, the 64-MiB object caps apply to `RefLog` and `RefSnapshot`, not blob bodies (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp:93-102`). Condemned resurrection weights the actual header plus arbitrary source size, and explicitly admits an object heavier than the entire capacity exclusively (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:716-728`). The semaphore remains safe; the problem is that operators are given a false rationale and may expect normal 64-MiB-per-worker behavior for larger blobs.

The same truthfulness sweep also missed the production comment claiming mutable part files “ride inside the ref payload,” which no longer exists (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1347-1352`).

Describe 64 MiB as the chosen default per-task budget, explain overweight exclusive admission, and replace the stale ref-payload explanation with the ref-to-manifest model.

## Verified composition points

- The fan-out fix is sound: handle capacity is reserved before scheduling, every scheduled task is recorded through `enqueueAndGiveOwnership`, and the scope guard drains on every exit (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1688-1704`, `:1713-1730`).

- The leadership-baton fix is sound: the first responsibility entry is allocated before `leader_active` is published, and an allocation failure removes the still-queued item (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1010-1032`). Carved responsibility storage is likewise reserved before pending items are popped (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1332-1348`).

- A raw over-cap tail object fails recovery fast. `openObject` throws `CORRUPTED_DATA` for raw and compressed over-cap bodies (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp:384-403`); corruption is absent from the transient classifier and is rethrown (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:64-80`, `:552-560`). The read-only recovery path re-lists only when GET reports a vanished object, not when decoding throws (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:721-765`).

- Current-build `set_published_at` encode → decode → apply preserves the same ref name, expected manifest, and timestamp (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp:88-112`, `:170-223`; `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:236-258`).

- Removal classification and chunking compose correctly for production mutations: the canonical predicate detects `RemoveNamespace` by op inspection (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp:328-330`), removal is singleton-carved (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1306-1320`), and normal chunks split before exceeding the op cap (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1397-1447`). `SetPublishedAt` remains included in the state-growth admission check (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1505-1518`).

- Apart from the missing seal fact above, recovery context fields are filled before installation; exceptions before or during seal publication leave `recovered == false`, and successful installation sets it last (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:445-468`, `:496-547`, `:620-635`).

This was a static, read-only review as requested; no build or tests were run.

**VERDICT: SOUND WITH FIXES — replace the ineffective T13 memory-bound test, complete/reconcile `RecoveryResult::sealed_from`, reject legacy `"pl"` fields explicitly, and correct the remaining cap/payload documentation.**
## Fix-verification passes (2026-07-24) {#fix-verification-passes}

### Pass 1 over the round-2 fix commits — verdict: FIXES INCOMPLETE (2 residuals) {#verify-pass-1}

## Verification

1. Important — partially resolved; regression guard remains incomplete

The footprint itself is appropriate. `decodedRefLogTxnFootprint` counts the transaction namespace, `RefOp` vector storage, and every owning ref-name string; all other operation fields are fixed-size values. It deliberately uses decoded content rather than compressed object bytes. Ignoring allocator slack/capacity makes it a deterministic proxy, but does not weaken its ability to distinguish one decoded transaction from a whole resident tail ([CasRefProtocol.cpp:119](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:119)).

All three current loops correctly bracket each decoded transaction around `applyOne`:

- Writer ledger: [CasRefLedger.cpp:426](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:426)
- Standalone/orphan-sweep recovery: [CasRefProtocol.cpp:754](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:754)
- Fsck snapshot oracle: [CasFsck.cpp:248](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:248)

`MaterializingControlExceedsMemoryBound` is genuinely equivalent to the feared decoded-tail regression: it GETs and decodes all transactions, retains them in `resident_txns`, reports all footprints before applying any, and only releases the total afterward ([gtest_cas_recovery_streaming.cpp:260](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:260)).

However, the production regression guard is incomplete:

- `LongTailReplaysUnderMemoryBound` calls the free `recoverRefTable`, which delegates to `recoverRefTableDetailed`; it does not exercise `CasRefLedger::ensureRefTableRecovered` ([gtest_cas_recovery_streaming.cpp:224](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:224), [CasRefProtocol.cpp:789](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:789)).
- `OrphanSweepAndFsckSameBound` covers only the free recovery and fsck paths ([gtest_cas_recovery_streaming.cpp:427](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:427)).
- Therefore, the ledger can regress to whole-tail materialization while all existing bounds and the test-local control remain green. “By construction” establishes that today’s code streams, but is not an adequate regression guard for the original validation finding.
- More generally, accounting remains manually coupled to the production loop. The streaming assertion is upper-bound-only, so deleting the reports gives a zero peak that still passes ([gtest_cas_recovery_streaming.cpp:240](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:240)). Likewise, moving transactions into a vector while leaving `+/-` scoped to the decode iteration can under-report the retained vector. The RED control proves the accountant discriminates when correctly driven, not that every production materialization regression must drive it correctly.

Thus the current implementations are streaming, but finding 1’s load-bearing regression guard is not fully resolved.

2. Minor — original seal fold fixed, but the new runtime copy becomes stale

The reported recovery bug is fixed correctly:

- After a recovery seal commits, both `newest_snapshot_id` and `sealed_from` move to the new seal ([CasRefLedger.cpp:539](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:539)).
- `installRecoveryResult` copies `sealed_from`, with an honest statement that it has no hot-read consumer ([CasRefLedger.cpp:628](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:628)).
- Clean recovery pins `nullopt`, and the new unclean case pins the seal value together with its ID ([gtest_cas_recovery_streaming.cpp:526](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:526), [gtest_cas_ref_writer.cpp:2861](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_writer.cpp:2861)).

But the fix introduced an introspection inconsistency. The runtime field is documented as belonging to the snapshot currently named by `newest_snapshot_id`, with `nullopt` for an ordinary snapshot ([CasRefLedger.h:367](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h:367)). Later publication of an ordinary Live snapshot advances `newest_snapshot_id` without clearing `rt.sealed_from` ([CasRefLedger.cpp:2116](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2116)); removed-snapshot publication does the same ([CasRefLedger.cpp:2330](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2330)).

Consequently, after recovering from a seal and later publishing a normal or removed snapshot, `sealedFromForTest` returns the predecessor seal’s value while `newestPublishedSnapshotIdForTest` names a non-seal. This has no current data-plane effect, but makes the new inventory/introspection contract and comments false. Finding 2’s immediate recovery case is resolved, but the added runtime representation is not completely maintained.

3. Minor — fully resolved

The removed `"pl"` field is now explicitly rejected:

- The generic op-record reader rejects it for every recognized operation kind with `CORRUPTED_DATA` and a truthful message naming the removed field ([CasRefLogFormat.cpp:201](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp:201)). The retired `set_payload` word is independently rejected as unknown ([CasRefLogFormat.cpp:35](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp:35)).
- Snapshot record decoding rejects `"pl"` before dispatching committed versus precommit rows ([CasRefSnapshotFormat.cpp:286](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp:286)).
- Both legacy wire locations have negative decoding pins ([gtest_cas_ref_log_format.cpp:188](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_log_format.cpp:188), [gtest_cas_ref_snapshot_format.cpp:65](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_snapshot_format.cpp:65)).

I found no remaining decoder that tolerates `"pl"` in either of its former wire locations. Tolerant handling in unrelated meta/trailer records is not a legacy-field path because `"pl"` was never valid there.

4. Minor — fully resolved

The 64-MiB rationale is now truthful:

- The setting describes 64 MiB as the chosen default per-task budget, explicitly states that blob bodies are uncapped, and explains exclusive overweight admission ([ServerSettings.cpp:156](/home/mfilimonov/workspace/ClickHouse/master/src/Core/ServerSettings.cpp:156)).
- The pool declaration and constant use the same rationale ([CasBlobUploadPool.h:126](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.h:126), [CasBlobUploadPool.cpp:183](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.cpp:183)).
- This matches actual behavior: an overweight body drains normal holders and runs exclusively ([CasBlobUploadPool.cpp:78](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.cpp:78)), while the materialized body is weighted by its actual header plus source size ([CasPartWriteTxn.cpp:709](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:709)).
- The stale ref-payload explanation is correctly replaced by the ref-binding-to-manifest reachability model ([CasPool.cpp:1341](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1341)).

No other material behavior change, weakened existing test, or new correctness defect was found in the fix commits beyond the ledger memory-guard gap and the newly stale runtime `sealed_from` introspection field. Per instruction, this was static and read-only; no builds or tests were run.

**VERDICT: FIXES INCOMPLETE (1: writer-ledger/independent memory regression guard; 2: runtime `sealed_from` is not cleared when a later ordinary or removed snapshot becomes newest).**
### Pass 2 over the residual fix commit 330d52e8c96 — verdict: FIXES VERIFIED {#verify-pass-2}

Both residuals are resolved in the final `cas-gc-rebuild` tree. No new correctness issue or test weakening was found.

1. Memory-bound regression guards

- The footprint is fixture-derived and nonzero: `seedBigTail` calculates the maximum and total using `decodedRefLogTxnFootprint` for actual transactions containing 250 operations ([gtest_cas_recovery_streaming.cpp:88](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:88), [CasRefProtocol.cpp:119](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:119)).
- Free recovery is two-sided: peak must be at most twice the largest transaction and at least the largest actual transaction footprint ([gtest_cas_recovery_streaming.cpp:235](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:235), [gtest_cas_recovery_streaming.cpp:246](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:246), [gtest_cas_recovery_streaming.cpp:252](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:252)).
- The orphan-sweep/free-recovery and fsck-oracle assertions are likewise two-sided ([gtest_cas_recovery_streaming.cpp:464](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:464), [gtest_cas_recovery_streaming.cpp:466](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:466), [gtest_cas_recovery_streaming.cpp:468](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:468), [gtest_cas_recovery_streaming.cpp:478](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:478), [gtest_cas_recovery_streaming.cpp:480](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:480), [gtest_cas_recovery_streaming.cpp:482](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:482)). The fsck path genuinely enters its independent oracle loop ([CasFsck.cpp:248](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:248)).
- The new ledger test genuinely drives the ledger’s own recovery, not the free function. `Pool::tailSinceSnapshotCountForTest` forwards directly to `CasRefLedger` ([CasPool.cpp:1525](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1525)); the ledger accessor calls `ensureRefTableRecovered` itself ([CasRefLedger.cpp:1976](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1976)); that method contains its own decode/apply loop and report calls ([CasRefLedger.cpp:418](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:418), [CasRefLedger.cpp:436](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:436)).
- Its fixture proves the whole tail exceeds the bound, verifies all 24 transactions replayed, and enforces both peak bounds plus zero live accounting afterward ([gtest_cas_recovery_streaming.cpp:498](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:498), [gtest_cas_recovery_streaming.cpp:510](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:510), [gtest_cas_recovery_streaming.cpp:523](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:523), [gtest_cas_recovery_streaming.cpp:525](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:525), [gtest_cas_recovery_streaming.cpp:531](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_recovery_streaming.cpp:531)).

The lower bounds are real decoded-transaction footprints, not zero or nominal constants. Removing the relevant accounting leaves an insufficient peak and fails the corresponding guard.

2. `sealed_from` lifecycle

- The runtime contract explicitly pairs `sealed_from` with `newest_snapshot_id`, requiring `nullopt` for ordinary snapshots ([CasRefLedger.h:366](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h:366)).
- Recovery seals still install both the seal ID and its own bound together ([CasRefLedger.cpp:539](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:539), [CasRefLedger.cpp:636](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:636)).
- Ordinary publication now adopts `snap.sealed_from`; `snapshotOf` leaves that optional unset, so advancing to an ordinary Live snapshot clears the predecessor seal value under the same state lock ([CasRefProtocol.cpp:443](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:443), [CasRefLedger.cpp:2116](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2116)).
- Removed publication similarly copies the default-null `removed_snap.sealed_from` when adopting the removal snapshot ([CasRefLedger.cpp:2323](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2323), [CasRefLedger.cpp:2335](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2335)).
- The ordinary test proves the complete transition: recover seal `{2, UINT64_MAX}` with `sealed_from={2,1}`, publish epoch-3 ordinary state, then observe `nullopt` ([gtest_cas_ref_writer.cpp:2928](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_writer.cpp:2928), [gtest_cas_ref_writer.cpp:2936](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_writer.cpp:2936), [gtest_cas_ref_writer.cpp:2940](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_writer.cpp:2940)).
- The removed variant proves the corresponding seal-to-Removed transition and `nullopt` result ([gtest_cas_ref_writer.cpp:2976](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_writer.cpp:2976), [gtest_cas_ref_writer.cpp:2984](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_writer.cpp:2984), [gtest_cas_ref_writer.cpp:2987](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_ref_writer.cpp:2987)).

3. New regressions

The commit changes production behavior only by maintaining the documented introspection field at the two missing publication sites. Both updates occur only after successful publication and while holding `state_mutex`; the ordinary path also respects the existing monotonic-adoption guard. Existing tests were not relaxed—only lower bounds and targeted sequence coverage were added.

Review was read-only and static as requested; no builds or tests were run.

**FIXES VERIFIED**