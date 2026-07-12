# CAS Stabilization & Cleanup Iteration — Design

Date: 2026-07-12. Branch: `cas-gc-rebuild` (feature maintained in a fork; merge-base with `upstream/master`: `378a25bb3b1`).

## 1. Context and inputs

Two independent evidence sources drive this iteration:

1. **Umbrella multi-perspective review** (2026-07-12, 14 reviewers over `378a25bb3b1..HEAD`, C++ only). Produced 2 blocker-class findings, ~12 major, ~10 minor, plus hygiene. Top items verified against `HEAD`.
2. **CAS archaeology report** (`tmp/archaeology/00-REPORT.md`, 8 volumes). Produced an 18-item risk register (§9), a duplication table (§8, D1–D10), a dead-code inventory (Appendix D), and a recommended cleanup sequence (§13).

One umbrella finding was **retracted** on owner review: the replica fetch-by-relink "RBAC bypass" is *not* CAS-specific — ordinary `ReplicatedMergeTree` interserver part fetch already has the identical trust model (a malicious/MITM peer on the interserver channel can serve arbitrary part bytes the receiver adopts). Table-level RBAC never defended against a hostile peer; the interserver channel is the trust boundary. Action is a documenting comment, not a fix. (See [[feedback_cas_relink_trust_model]].)

## 2. Goals and non-goals

**Goals.** Land every item that is (a) genuinely dangerous or a clear correctness fix, (b) an improvement with no behavioral side effects, (c) a defect with a clean fix that does not add complexity or oddness, or (d) a change that makes the code cleaner/simpler — subject to the footprint constraint below.

**Non-goals.** No large structural refactors (`CasGc.cpp` split, `Cas::Store` de-god-classing). No new abstractions or config knobs that lack a consumer. No pre-emptive generality. No behavioral change whose only justification is taste.

## 3. Governing constraint — the three-ring footprint model

The feature is maintained in a fork. Every edit to a file shared with upstream is a future rebase conflict. All work is classified by *where it lands* and, for shared files, by *the direction of the delta*:

- **Ring 0 — inside `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (and `Core/`).** Free rein. Zero upstream-conflict risk. The bulk of this iteration.
- **Ring 1 — CA-specific lines in shared files:** append-only enum/`ProfileEvents`/`CurrentMetrics`/system-table entries, the `registerContentAddressedMetadataStorage` function, CA `SYSTEM`-command case handlers. Upstream never edits these lines, so they are effectively conflict-free. Include the clean ones.
- **Ring 2 — shared hot upstream code** (`MergeTask`, `MutateTask`, `PocoHTTPClient` core, `S3::Client`, `DiskSelector`, generic `IMetadataTransaction`/`DiskObjectStorageTransaction` logic). **Touch only when the delta shrinks or neutralizes the diff we already carry, or fixes a real bug, or makes the code simpler/safer/better-reused.** Never add new surface that makes the shared-file footprint *grow*.

The test for a Ring-2 change: *does it make the fork's existing diff against upstream smaller, safer, or more idiomatic?* If yes, do it. If it merely adds more CA-shaped lines to a hot upstream file, defer it (Group G / upstream).

## 4. Work items

Each item lists: **[Ring]** · location · problem · fix · verification. Evidence tags: `U#n` = umbrella finding, `A§9#n`/`A-D#`/`A§13` = archaeology.

### Group A — Correctness fixes (dangerous, clean)

**A1. `~Store()` teardown can `abort()` the process.** **[Ring 0]** `CasStore.cpp` (`~Store()` ~435-462, `scheduleRemount()` ~666-692), `CasServerRoot.h` (`MountLeaseKeeper`). `U#1`, lifetime.
A lease-renewal failure firing during teardown calls `on_lost → scheduleRemount()`, which guards on `remount_running`/`background_watermark` but **not** on `remount_stop`; it re-arms `remount_thread` after the destructor's only join. `ThreadFromGlobalPoolImpl`'s move-assign and destructor both `abort()` on an un-joined thread → hard `std::terminate`. Verified against `HEAD`.
Fix: add a `remount_shutting_down` flag set under `remount_thread_mutex` at the very top of `~Store()`; make `scheduleRemount()` check it first and refuse to spawn; re-join `remount_thread` after `mount_keeper->stop()`. Additionally give `MountLeaseKeeper` its own `~MountLeaseKeeper() override { stopBackground(); }` so the renewal thread is always joined before its `std::function` members are destroyed.
Verify: new gtest driving `scheduleRemount()`/background-renewal against a concurrent `Store` teardown (the real path — existing `gtest_cas_store.cpp` only drives `tryRemountOnce()` synchronously).

**A2. `RunFileReader::next()` heap OOB read.** **[Ring 0]** `CasRunFile.cpp` (`installBlockFrame` ~360-389, `next()` ~435-459). `U#3`.
`rec_count` is read at offset 4, *before* the CRC-covered `payload` span, so corruption of that un-checksummed field is undetected; `next()` then raw-indexes `cur_block` via an unchecked `le32at` lambda (`s[off+i]`), reading past `size()` → UB / SIGSEGV on the GC fold and manifest-decode paths, violating the file's own documented fail-closed contract (`CasRunFile.h:92`). Verified against `HEAD`.
Fix: include the whole block head (incl. `rec_count`) in the block CRC; make `next()` use the bounds-checked `le32of`-style reader against `cur_block`; assert exact consumption (`cur_block_pos == cur_block.size()`) after the block's last record.
Verify: corruption test — inflate `rec_count` / bit-flip block head, assert `CORRUPTED_DATA` thrown, never a crash.

**A3. `flushRefBatch` permanently wedges the ref-queue leader on an uncaught throw.** **[Ring 0]** `CasStore.cpp` (`~1212-1276`, `:1338`, `:1514`). deep-audit, `U#4`.
`resolveByExactGet`/`putIfAbsentControlled` can throw `CORRUPTED_DATA`; both sites are unguarded and `appendRefOps` has no `try/catch` around `runRefQueueLeader`. A throw skips `rt->leader_active = false` and `rt->cv.notify_all()`, hanging the namespace's whole mutation path until restart. The neighboring "durably-committed txn fails to apply" catch already does the correct recovery — apply the same shape here.
Fix: wrap the two sites (or the `flushRefBatch` body) in `catch(...)` that completes remaining `rt->pending`/carved survivors with the exception, resets `leader_active`, notifies `cv`, and rethrows.
Verify: gtest injecting a `resolveByExactGet` "different bytes" throw with a second caller queued behind the leader.

**A4. EDGE-BEFORE-OBSERVE invariant guarded only by `chassert`.** **[Ring 0]** `CasBuild.cpp` (`observeAndAdmit` ~247, ~270). `U#12`.
`chassert(precommitted)` is compiled out in release; the ADOPT path's safety argument depends on it. A future wiring/retry bug could silently adopt an existing incarnation without watermark protection → later dangling-reference/data-loss with no production signal.
Fix: promote to a real `throw Exception(LOGICAL_ERROR, ...)` in the ADOPT (existing-incarnation) branch.
Verify: existing build gtests stay green; add a negative test asserting the throw.

**A5. Ordinary-engine detached parts leak into a spurious namespace.** **[Ring 0]** `PartPathParser.cpp` (`findPartDirComponent` ~102-125), `ContentAddressedMetadataStorage.cpp` (`route` ~591-603). `U#6`.
For a non-Atomic DB, the rightmost part-dir-shaped component wins, so `detached/attaching_all_0_0_0/...` parses to `table_uuid="…/detached"` — a namespace distinct from the real table's. `DROP TABLE` never cleans it, so any part detached at drop time is a permanently orphaned live ref. Reviewer verified via an extracted+compiled parser.
Fix: in the non-Atomic fallback, special-case `kDetachedDirName` — anchor on `detached` when scanning right-to-left.
Verify: extend `CaPartPathParser` tests with the Ordinary-engine detached form (existing test only covers the Atomic form).

**A6. CAS startup capability probe bypasses `skip_access_check`.** **[Ring 0 for the fix]** `CasStore.cpp` (`Store::open`/`runCapabilityProbe` ~213-236), `ContentAddressedMetadataStorage` `startup()`. `U#5`, compat.
The write/delete probe runs unconditionally inside `startupImpl()`, before the `skip_access_check`-gated `checkAccess()`, so a mistyped bucket / transient DNS blip at boot throws where operators expect the standard "start now, fix later" path.
Fix (Ring 0 only): thread `skip_access_check` into `Store::open` and skip `runCapabilityProbe` when set, mirroring `checkAccess()`. The generic `DiskSelector::initialize()` per-disk isolation gap is **out of scope** (Ring 2, pre-existing upstream gap — Group G).
Verify: unit-level assert that open with `skip_access_check` performs no probe I/O.

**A7. Manual `SYSTEM … GC` uses a throwaway `Gc` instance and races the background loop.** **[Ring 0]** `CasGcScheduler.cpp` (`runOneRoundNow` ~170-174 vs `loop` ~176-244), `ContentAddressedMetadataStorage.cpp` (lazy `gc_scheduler`). `U#7`, `A§9#10`.
`runOneRoundNow` constructs a fresh `Cas::Gc gc(store, gc_id)` per call, discarding the stable-observer state the lease-steal protocol requires (so the command can never recover a dead-incumbent lease), and takes no lock against the live `loop()` thread (two rounds under one `gc_id`). Also folds in the unguarded lazy `gc_scheduler` creation.
Fix: reuse one persistent `Gc` instance for both `loop()` and `runOneRoundNow`; serialize the manual round against the background round under the scheduler `mutex`; guard `gc_scheduler` creation with a mutex.
Verify: gtest running a manual round concurrently with a started scheduler; a dead-incumbent-lease steal test across two rounds.

**A8. Enum decoded without range check in `CasGenerationSeal`.** **[Ring 0]** `CasGenerationSeal.cpp` (~122, ~140). `U`-minor.
`RefNsCleanupState`/`TokenType` `static_cast` from wire bytes with no validation, unlike every sibling codec; `RefNsCleanupState` feeds GC decision logic.
Fix: add the same explicit range check the sibling codecs use before the `static_cast`, throwing `CORRUPTED_DATA`.
Verify: decode-corruption test for the out-of-range value.

**A9. `dropRefBestEffort` swallows rollback failure with no logging.** **[Ring 0]** `CachedPartFolderAccess.cpp` (~283-296), `ContentAddressedTransaction.cpp` (commit catch ~319-353). lifetime.
A correlated backend outage during partial-commit rollback can leave a permanently-live phantom ref (GC reclaims only *unreferenced* objects). The swallow has no log trail, unlike every other best-effort swallow in the codebase.
Fix: call `tryLogCurrentException` on the swallowed path; consider surfacing it as a countable GC-round anomaly.
Verify: fault-injected rollback failure asserts a log/counter.

**A10. `suppress_destructive` computed twice from mutable state.** **[Ring 0]** `CasGc.cpp` (~635, ~1149). `A§9#7`.
Computed independently in `fold` and the round from mutable `report.anomalies`; a future edit could make fold suppress deletes while the round doesn't (over-delete class).
Fix: compute once in `fold`, thread through `FoldResult`.
Verify: existing `gtest_cas_gc_*` stay green.

### Group B — Performance / operability (clean improvements)

**B1. CAS read path re-parses the full part path per metadata call.** **[Ring 0]** `ContentAddressedMetadataStorage.cpp` (`route`/`liveNamespace` ~562-609), `PartPathParser.cpp` (`splitNonEmpty` ~6-26). `U#9`.
Every `existsFile`/`getFileSize`/`getStorageObjects`/… independently re-runs `parsePartFilePath`+`route` (~10-15 allocations), several times per file-open; `splitNonEmpty` copies each component.
Fix: resolve the route once per logical file-open and reuse it across the metadata calls (or memoize keyed on the raw path for the read-setup duration); `std::move` in `splitNonEmpty`.
Verify: request/allocation-count oracle in the existing cache-path test style.

**B2. `getView` hit path takes a per-disk global mutex for a debug journal.** **[Ring 0]** `CachedPartFolderAccess.{h,cpp}` (`explain_mutex`/`explain_map` ~124-128, `recordDecision` ~310-324). `U#9`.
The cache-*hit* path unconditionally takes `explain_mutex` (one per disk, every reader) and allocates `cacheKey()` twice, purely to maintain a "Test/log-only decision journal."
Fix: make the `explain` journal opt-in (setting, off by default); compute `cacheKey()` once per `getView` and reuse it.
Verify: existing cache tests; assert the journal is empty when disabled.

**B3. Per-disk GC health signal.** **[Ring 0 + Ring 1]** `StorageSystemContentAddressedMounts.{h,cpp}` (CA system table), `CasGcScheduler`. `U#11`, operability.
The two GC `CurrentMetrics` (`CasGcIsLeader`, `CasGcPendingReclaimEntries`) are process-global and clobbered with ≥2 CAS disks; there is no "seconds since last successful round" signal and wedge state is only reachable via a `ForTest` accessor.
Fix: expose per-disk `is_leader`, `pending_reclaim`, `last_success_age`, `wedged_namespace_count` as columns on `system.content_addressed_mounts` (CA-owned surface). Retire or document the process-global gauges.
Verify: system-table read reflects per-disk state.

**B4. Late-Predecessor-PUT observability counter.** **[Ring 0 + Ring 1]** `CasStore.cpp` (`trySnapshotPublishOnce` ~1690-1706), `ProfileEvents.cpp` (CA block). deep-audit.
The spec-acknowledged residual ref-loss race is mitigated only by a grace-age window; the spec's required diagnostic counter is unimplemented, so the risk is currently unmeasurable.
Fix: add `ProfileEvents::CasRefLatePredecessorObserved`, incremented when a post-fence response for a stale-epoch request is observed.
Verify: fault-injected late response increments the counter.

### Group C — Simplification / dedup (cleaner, better reuse)

**C1. Token-policy helper inside `ObjectStorageBackend`.** **[Ring 0]** `CasObjectStorageBackend.cpp`. `A-D9`, `A§13.2#2`.
Native-ETag vs emulated vs GCS-generation token logic is scattered across `head`/`list`/`casPut`/`deleteExact`/`supportsListTokens` (the emulated list-vs-head mismatch already fired once).
Fix: consolidate into `tokenForHead`/`tokenForList`/`tokenMatches` helpers.
Verify: the parameterized backend contract suite (already exists).

**C2. Shared delete-outcome classifier + shared LIST-pagination iterator.** **[Ring 0]** `CasGc.cpp`, `CasFsck.cpp`, `CasOrphanManifestSweep.cpp`, `CasStagingSweeper.cpp`. `A-D4`/`A-D5`, `A§13.2#3`.
The Deleted/NotFound/TokenMismatch/marker classification and the LIST/cursor loop are re-implemented at ~6 and ~10 sites respectively (`CasFsck` already factors a local `listAll`). Semantic/counter drift risk.
Fix: one callback-based `forEachListedKey(backend, prefix, cb)` and one shared delete-outcome classifier, migrate the GC/sweep/fsck sites onto them.
Verify: full GC gtests stay green.

**C3. Define `Layout::blobKey`/`parseBlobKey` where declared.** **[Ring 0]** `CasLayout.h` (declared) vs `CasBuild.cpp` (defined). `A§6.1`.
An include-cycle workaround puts the blob-key grammar out of line in `CasBuild.cpp`; a reader of `CasLayout.h` can't find it.
Fix: move the definitions to a new `CasLayout.cpp` (or otherwise break the cycle) so declaration and definition co-locate.
Verify: build; layout gtests.

**C4. Unify `existsDirectory`/`listDirectory` shape dispatch.** **[Ring 0]** `ContentAddressedMetadataStorage.cpp` / `PartPathParser`. `A-D8`, `A§13.2#7`. (Med.)
The load-bearing fixed dispatch order (`shadow → atomic-shard → table-uuid → part → subdir → generic`) is implemented twice and must be kept in sync by hand.
Fix: one routing table both paths consume.
Verify: `gtest_ca_wiring` shape-dispatch cases.

**C5. Encapsulate the whole-part-transaction rule (remove the 6 `isContentAddressed()` guards).** **[Ring 2 — net-shrinks the diff]** `MergeTask` (×3), `MutateTask`, `MergeProjectionPartsTask`, `MergeTreeDataWriter`, `IMergeTreeDataPart`. `U#13`/`A§9#2`/`A-D2`. (Med risk.)
`if (!isContentAddressed()) begin/commitTransaction()` is copy-pasted across 6 MergeTree files; forgetting it in a new part/projection path is a silent-corruption class (B58). This is a **Ring-2 change that removes surface**: the encapsulation deletes the 6 duplicated conditionals rather than adding new ones.
Fix: make `begin`/`commitTransaction` no-ops on borrowed projection storage (or centralize the decision on the storage object) so callers never branch on `isContentAddressed()`. Use the minimal-diff variant; do not broaden into an unrelated transaction refactor.
Verify: existing projection tests stay green; add a test that a projection sub-part rides the parent whole-part transaction on a CA disk. Careful review of merge/mutate paths required.

### Group D — Remove all old stuff (dead code, stale comments, vestigial state)

Owner decision: **remove all** flagged vestigial state, including format constants. All Ring 0 unless noted. Evidence: `A§9#9`, `A§13.1`, Appendix D.

**D1. Vestigial state/format constants.** Remove: `root_shards` from `PoolConfig`/`PoolMeta` (+ its factory default); `_precommits` layout modeling + `isPrecommitNamespace` (`CasLayout.h:462-509`, `CasInstrumentedBackend.cpp:131`); `fence_seq` from `GcState` (+ its one inspect read); `ShardCoverage::folded_cursor` + `::incarnation`; seal `classification=3`; single-value `ObjectKind` enum (if cleanly removable); `PartFolderView::fileSize` documented-unreachable mutable branch.
Verify: layout/format gtests; full build.

**D2. Dead counters/events.** **[Ring 1]** Delete the ~14 zero-increment `ProfileEvents` (`ProfileEvents.cpp`): `CasShardBatchedMutations`, `CasShardBatchFlushes`, `CasShardBatchScopeCuts`, `CasShardQueueWaitMicroseconds` (removed shard-mutation-queue); `CasManifestBackpressureCount`, `CasManifestBackpressureMicroseconds`, `CasManifestHardLimitExceeded`; `CasPartFolderViewEvictions`; `ContentAddressedGenerationResurrectionsTotal`, `ContentAddressedDuplicateGenerationBytes`, `ContentAddressedTombstonesTotal`, `ContentAddressedGenerationsObserved`, `ContentAddressedHashesObserved`, `ContentAddressedOrphanBytesEstimate` (pre-rebuild GC). Verify each has zero increment sites before removal; check no soak dashboard depends on them. Also remove the `GcSnapPersist` event husk and the always-0 `CasEvent::round` field (5 write sites).
Verify: `gtest_cas_event_log`; build.

**D3. Stale comments / anchors / debris.** Re-point or remove comments naming the deleted `Store::shardOf` (`CasGcShardPlan.h:23`, `CasBuild.h:98,103`); the `_watermark` classifier branch (`CasInstrumentedBackend.cpp:129`) + stale watermark comment (`CasStore.cpp:263`); stale key-layout comment (`CasGcOutcomes.h:33-37`, `fence_seq` keying) + dead line anchors (`CasGcCursorKey.h`); `report.candidates += 0;`, the empty `if (!src_st.build) {}` branch, `exists()` refactor debris; the stale `notYet()` message (cache-over-CA is handled now); stale `ContentAddressedLog` column docs (`manifest_expand`/`strip`/`pack`/`tree` never emitted) + `SystemLog.h:21` description.
Also **[Ring 1]** `checkContentAddressedDiskRestrictions` in `MergeTreeData` is a CA-specific no-op the branch added — remove it (net-reduces the CA footprint in a shared file) or mark intentionally empty.
Verify: build; no test needed for comment-only edits.

**D4. Relink trust-model comment.** **[Ring 0, + one Ring-1 line]** Add an inline comment at `ContentAddressedMetadataStorage::adoptPartFromManifest` (and optionally at the `DataPartsExchange` relink call site) documenting that the trust model equals ordinary `ReplicatedMergeTree` interserver fetch — the interserver channel is the trust boundary, not a per-blob ACL. No code change.

**D5. Test-side migration cleanup.** **[Ring 0, tests]** Drop the legacy shard-key branch from `isRefWriteKey`/`isShardManifestPath` in the wiring test; rename or repurpose `gtest_cas_dangling_precommit.cpp` (reduced to a 40-line `ShardCoverage` codec round-trip — the name no longer matches its content). Keep the two `DISABLED_` tree-model tests (deliberate documentation).

### Group E — CA-specific shared-file lines (Ring 1, conflict-free)

**E1. Split the `GC REBUILD` access right + require an explicit disk.** **[Ring 1]** `AccessType.h:351` (append a new right), `InterpreterSystemQuery.cpp` (CA cases), `ParserSystemQuery.cpp` (CA grammar). `U#8`, `A§9#8`.
Add `SYSTEM_CONTENT_ADDRESSED_GC_REBUILD`, distinct from the benign per-round `SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION`; require an explicit disk for `REBUILD [FORCE]` (no all-disks broadcast when the disk is omitted).
Verify: an `01271`-style privilege test; parser test for the required-disk grammar.

**E2. Config-key and naming fixes.** **[Ring 1]** In `registerContentAddressedMetadataStorage` (`MetadataStorageFactory.cpp`): rename the member `gc_max_conditional_put_bytes_` → `gcs_…` (`A§9#11`); make the missing-`server_root_id` error a proper `DB::Exception` (`NO_ELEMENTS_IN_CONFIG`) mirroring the `metadata_type` check; reconcile the `cas_`-prefix inconsistency across the ~4 prefixed keys. `root_shards` default alignment is moot once D1 removes it.
Verify: config-parse gtests / build.

**E3. System-surface polish.** **[Ring 1]** `system.content_addressed_mounts`: type `server_uuid` as `DataTypeUUID`, `started_at_ms`/`expires_at_ms` as `DataTypeDateTime64(3)`. Reconcile the two system logs' enabled-by-default config vs the `SystemLog.h` "off by default" comment. Consider a verb-first alias for `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION` (optional, pre-release naming).
Verify: system-table schema test.

### Group F — Repository hygiene

**F1. Delete `poc/cas_mergetree/` entirely.** **[outside feature, standalone]** Orphaned standalone PoC (1463 lines, `cas.cpp`/`cas.h`/`tests.cpp` + README/CMake), fully superseded by the real `Core/` implementation, referenced by nothing, with generic class names (`GC`/`Engine`/`Catalog`) that pollute symbol search. Pure deletion; zero conflict.

## 5. Ring-2 items done because they shrink/fix, not grow

These edit shared files but the delta is a net-improvement to the diff we already carry:

- **C5** (above) — removes the 6 duplicated guards.
- **`copyS3File` `message_format_string`** — restore `PreformattedMessage::create(...)` at the two throw sites the branch rewrote (`copyS3File.cpp:68-74,126-138`), matching `WriteBufferFromS3.cpp`. Repairs lines already modified; restores `system.text_log`/`system.errors` grouping for all S3 copy/backup errors. `U`-minor.
- **412/PreconditionFailed detection** (archaeology duplication `A-D1`) — consolidate the `getExceptionName()=="PreconditionFailed" || message.find(...)` check duplicated across `S3::Client::RetryStrategy`, `removeObjectIfTokenMatches`, `copyObjectConditional` into one `S3Exception::isPreconditionFailed()`. Fewer modified lines, one policy, kills the "differently-worded store misclassifies" bug class. `A§13.2#1`.
- **`Expect: 100-continue` scoping** — scope the default-on conditional-PUT negotiation (`PocoHTTPClient.cpp` ~623-683) to CAS-owned writes via a `WriteSettings` flag (as `s3_force_single_part_upload` already does), so it stops changing wire behavior for the pre-existing non-CAS Iceberg conditional-commit path. Bug fix on code the branch added; makes it safer. `U#10`, compat.

## 6. Explicitly deferred / excluded

- **`CasGc.cpp` split** (2267 lines → fold/deletion/cleanup/cursor/budget) and **`Cas::Store` de-god-classing** — large; separate refactor iteration.
- **`DiskSelector` per-disk isolation** — adds new generic logic to a shared file for a pre-existing upstream gap → Group G / upstream.
- **`DiskObjectStorageTransaction` part-path virtualization** — would add virtuals to shared `IMetadataTransaction`; grows Ring-2 surface. Leave the existing CA dependency as-is.
- **`removeFileIfExists` + `writeFile` out-of-order write drop** — dead code today (no live caller of `writeMetadataVersion`); note only.
- **`MultipleDisksObjectStorageTransaction` `shared_from_this` → `bad_weak_ptr`** — latent, not reachable (cross-disk CA copy hits `notYet` first); note only.
- **TOKEN⟹CONTENT startup probe** (`A§9#1`) — real design value, but adds a probe to the backend contract; evaluate separately (keep as an open item, not this iteration) unless it lands as a pure Ring-0 addition to the existing `Cas::Probe` battery. *(Candidate for promotion if cheap.)*

## 7. Parallel track G — carve generic fixes into separate upstream PRs

Non-blocking. These are already-committed Ring-2 bug fixes that are not CAS-specific; upstreaming them separately is the highest-leverage way to shrink the fork's long-term conflict surface. Candidates: `ThreadStatus` `parent_thread_group` retention (B90), `ReadBufferFromFileView` position fix (B115), `ReadBufferFromS3` cancel-stop (B117), `LocalObjectStorage` TOCTOU robustness (B38), `MergeTreeDeduplicationLog` null-writer fail-close (B37), `copyS3File` `message_format_string`, `Expect: 100-continue` (as an opt-in generic S3 feature), `S3Exception::isPreconditionFailed()`, GCS conditional dialect + GOOG4 signer, the generic conditional-S3-write plumbing.

## 8. Sequencing

Phased so each phase is independently buildable and testable, and so the highest-danger fixes land first:

1. **Phase 1 — crash/corruption fixes:** A1, A2, A3, A4 (+ their tests). These are the release-blockers.
2. **Phase 2 — remaining correctness:** A5, A6, A7, A8, A9, A10.
3. **Phase 3 — remove all old stuff:** D1–D5 (dead code/comments/counters/vestigial state). Pure subtraction; do before simplification so refactors don't preserve dead paths.
4. **Phase 4 — simplification / dedup:** C1, C2, C3, C4, and the Ring-2-shrinking items (§5: `copyS3File`, 412 helper, `Expect:100-continue`).
5. **Phase 5 — performance / operability:** B1, B2, B3, B4.
6. **Phase 6 — encapsulation + surface polish:** C5 (the med-risk Ring-2 guard removal, landed last with focused review), E1, E2, E3, F1.

Group G is tracked separately and not gated by these phases.

## 9. Testing strategy

- Every Group-A item ships with a failing-first regression test pinned to its finding.
- No `sleep`-based concurrency tests. For A1/A3/A7 use deterministic fault-injection / the instrumented backend where the existing suite does; where a real interleaving is unavoidable (A1 teardown-vs-remount), use a controlled second thread gated on an atomic, consistent with the one existing real-thread test.
- Group-D removals gated by full build + the existing format/codec/event gtests; confirm zero increment sites / no dashboard consumers before deleting a counter.
- C5 gated by the existing projection tests plus a new "projection sub-part rides the parent transaction on a CA disk" test.
- Ring-2 §5 items gated by the relevant IO/S3 unit tests; `copyS3File` and the 412 helper by S3 unit coverage.

## 10. Risks

- **C5** is the one med-risk behavioral change to hot merge/mutate paths; landed last, in its own phase, with focused review. If it destabilizes, it can be dropped without affecting the rest of the iteration.
- **D1** removes format constants (`root_shards`, `fence_seq`). Cheap now (pre-release, no-compat rule) and owner-approved, but touches pool-meta/gc-state codecs — covered by format gtests; must confirm no live consumer remains.
- **A2**'s CRC-coverage change alters the on-disk run-file framing. Pre-release, so acceptable; ensure writer and reader are updated together and old-format run files are not expected to survive (they are attempt-scoped and short-lived).
```
