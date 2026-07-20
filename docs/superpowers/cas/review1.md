> **✅ RESOLVED (2026-07-13).** This whole-branch umbrella review was acted on: the 2026-07-12
> stabilization iteration executed and landed fixes for findings 1 and 3–12 plus the minors. Finding 2
> (relink "RBAC/confidentiality bypass") was **retracted as not-a-bug** — the interserver channel is the
> same trust boundary an ordinary `ReplicatedMergeTree` part fetch has; documented in code, not "fixed".
> Only findings **13** (architecture: `CasGc` split, `Cas::Store` de-god-classing, `DiskObjectStorageTransaction`
> virtualization) and **14** (test-coverage gaps: `Mode::Native` contract row, `DiskObjectStorageTransaction`
> CA-dispatch test, real-thread concurrency tests), plus the `DiskSelector` per-disk isolation residual,
> remain open — tracked in [`BACKLOG.md`](BACKLOG.md) §9–§10. Kept here for the full finding narrative.

Review report: cas-gc-rebuild (378a25bb3b1..HEAD), C++ only

Summary

This branch adds content-addressed storage (CAS) for MergeTree over shared object storage — part files become content-addressed blobs, parts are described by manifests, and a leader-elected background GC (leases/heartbeats, sharded plans, one-pass rounds) reclaims unreferenced blobs, coordinating entirely through conditional object-store writes (no Keeper). It touches 250 C++ files (+59,695 / −80): ~32k lines of new CAS engine + integration code, ~28k lines of new gtests, and targeted insertions into ~80 shared upstream files (S3 IO stack, MergeTree, Disks, the SQL/ops surface).

Overall the new code is unusually disciplined — fail-closed by default, heavily documented, TLA+-gated, scarred by prior incident write-ups. The review nonetheless surfaced two blocker-class defects (a destructor that can abort() the process; a table-level RBAC/confidentiality bypass in the replicated relink path), a cluster of major correctness/robustness gaps, and clear pre-upstreaming hygiene work (drop the orphaned PoC, prune dead metrics, split into reviewable PRs). Verdict: request changes.

Reviewed scope

- Diff: git merge-base upstream/master HEAD (378a25bb3b1, v26.6 dev, 2026-06-06) .. HEAD (ea180d83d58); 1833 commits; C++ only.
- Main moving parts:
  - CAS core engine (src/Disks/.../ContentAddressed/Core/, ~35 new files): CasStore, CasGc, CasBuild, CasObjectStorageBackend, CasServerRoot, hashers/digests/codecs/layout, GC formats/shard-plan/seal, ref machinery (CasRefStateMachine, CasRefIntake, CasSingleWriterSlot, CasRunFile, CasRequestControl).
  - CAS integration (.../ContentAddressed/): ContentAddressedMetadataStorage, ContentAddressedTransaction, CachedPartFolderAccess, PartFolderView, PartPathParser, CasGcScheduler.
  - Shared IO/S3: WriteBufferFromS3, copyS3File, PocoHTTPClient, S3/Client, WriteSettings; new GCSConditionalDialect + GOOG4Signer.
  - Shared MergeTree: DataPartsExchange (replication fetch protocol), DataPartStorageOnDiskFull, MergeTreeData, DiskObjectStorageTransaction, MergeTreeDeduplicationLog.
  - SQL/ops surface: system.content_addressed_mounts, two system logs, new SYSTEM commands, 4 clickhouse-disks commands, +124 ProfileEvents.
  - poc/cas_mergetree/ (orphaned standalone PoC).

Blockers

1. Store::~Store() can abort() the process — a lease-renewal failure during teardown re-arms remount_thread after the destructor's only join
Risk score: 90 · Sources: concurrency, lifetime · Confidence: high (verified against HEAD)
Files/lines: CasStore.cpp:435-462 (~Store()), :666-692 (scheduleRemount()), :388-396 (on_lost wiring); CasServerRoot.cpp:751-760 (onRenewFailed→on_lost); Common/ThreadPool.h:311-323 (abort() on un-joined thread).
Evidence: ~Store() joins remount_thread first (step 1), then calls mount_keeper->stop() (step 2). The keeper's own background renewal thread is alive across both steps; on a renewal failure it invokes on_lost = [raw]{ raw->tripMountLost(); raw->scheduleRemount(); }. scheduleRemount() gates only on config.background_watermark and remount_running — never on remount_stop — so after step 1 (where remount_running is already back to false), it move-assigns a fresh ThreadFromGlobalPool into remount_thread. Both ThreadFromGlobalPoolImpl::operator=(&&) and its destructor call abort() when the target is still initialized(). Step 1's join has already passed, so this new thread is never joined → member destruction aborts. This is a real std::terminate, not a chassert.
Impact: Guaranteed process crash under a realistic interleaving — an ordinary fenced-out or transiently-failing mount lease during a DROP/disk-detach/server-shutdown. No test exercises the real scheduleRemount()/background-renewal path against teardown.
Proposed fix: Add a remount_shutting_down flag set under remount_thread_mutex at the very top of ~Store(); make scheduleRemount() check it first and refuse to spawn once set; re-join remount_thread after mount_keeper->stop() for safety. (Lifetime reviewer's related structural fix: give MountLeaseKeeper its own ~MountLeaseKeeper(){ stopBackground(); } so the renewal thread is always joined before its std::function members are destroyed — CasServerRoot.h:286.)

2. Replicated CAS "fetch-by-relink" trusts wire-supplied manifest entries — an interserver peer can bind any live pool blob into a table it has no SELECT grant on
Risk score: 80 · Sources: security, tests, code-quality-integration · Confidence: high (verified against HEAD)
Files/lines: DataPartsExchange.cpp:240-249 (sender relink gate), :527-539 (receiver advertises pool UUID), :1106 (adoptPartFromManifest); ContentAddressedMetadataStorage.cpp:1142-1197 ("Sender identity is NON-AUTHORITATIVE… use ONLY the entries"); CasBuild.cpp:1069-1107 (promote revalidation).
Evidence: The receiver advertises its content_addressed_pool_uuid; a peer answering the fetch responds with a content_addressed_relink cookie + an arbitrary encoded PartManifest instead of bytes. adoptPartFromManifest decodes it, ignores the sender's identity, and republishes a ref in the receiver's namespace from the entries alone. The only gate is Build::promote's per-blob revalidation, which for a tokenless adopted leaf checks only (a) the blob key exists (head) and (b) it is not condemned — no check that the hash belongs to the fetched table, no ACL check. Blob keys are pool-global (POOL/blobs/<algo>/S/<hex>), not namespaced. So a malicious/MITM interserver peer can cause the receiver to durably bind a low-privilege table's part to the content of any live blob in the shared pool, including another tenant's higher-privilege table; any user with SELECT on the low-priv table then reads it. This defeats table-level GRANT/REVOKE for CAS tables sharing a pool. system.content_addressed_log.object_hash (readable by anyone with SELECT on that log) supplies the target hashes, removing the guess-the-hash precondition.
Notably, the relink path is on by default: advertised on try_zero_copy && !to_detached (default try_zero_copy=true), not gated on allow_remote_fs_zero_copy_replication like the legacy zero-copy path (DataPartsExchange.cpp:527 vs :547).
Impact: Confidentiality break / RBAC bypass in the exact multi-server shared-pool deployment this feature targets. Precondition is interserver-channel access (compromised node or unauthenticated/misconfigured interserver port) — a real but non-trivial bar; pre-CAS, such a peer could only forge bytes for the same table, so this is a genuine escalation to cross-table/cross-tenant.
Proposed fix: Don't treat wire-supplied entries as sufficient evidence for adoptEvidence/copy-forward — require the receiver to independently resolve the blob from a ref it can already see, or require server-side proof the sender owns a live committed ref naming that exact blob. At minimum, gate the whole relink capability behind an explicit off-by-default setting mirroring allow_remote_fs_zero_copy_replication, and document the pool-sharing implication.

Major issues

3. RunFileReader::next() raw-indexes block records using a rec_count no CRC covers — corruption of one 4-byte field turns a documented fail-closed error into a heap OOB read
Risk score: 65 · Source: code-quality-core · Confidence: high (verified against HEAD)
Files/lines: CasRunFile.cpp:360-389 (installBlockFrame), :435-459 (next()).
Evidence: The block CRC covers only payload = [off, block_end); rec_count is read at off=4, before that span (confirmed at HEAD). next() then walks cur_block_records times using a local le32at lambda that does s[off+i] with no bounds check (unlike every other decode in the file, which uses the throwing le32of). A rec_count inflated by even +1 makes next() read past cur_block.size() → UB / SIGSEGV on the GC fold and manifest-decode paths, violating the class's own contract (CasRunFile.h:92: "ANY CRC failure throws CORRUPTED_DATA — never a partial record"). Trigger is at-rest corruption of an un-checksummed field (defense-in-depth, not remotely reachable).
Proposed fix: Include the whole block head (incl. rec_count) in the CRC, and make next() use bounds-checked reads + assert exact consumption (cur_block_pos == cur_block.size()) after the last record.

4. Uncaught CORRUPTED_DATA in flushRefBatch permanently wedges a table's ref-queue leader — every waiter hangs until restart
Risk score: 68 · Source: deep-audit · Confidence: high
Files/lines: CasStore.cpp:1338, :1514, :1212-1276 (appendRefOps/runRefQueueLeader).
Evidence: resolveByExactGet/putIfAbsentControlled can throw CORRUPTED_DATA (byte-different object at a ref key); both call sites are unguarded, and appendRefOps has no try/r. A throw skips rt->leader_active = false and rt->cv.notify_all(), so the namespace's whole mutation path hangs forever. The neighboring "durably-committed txn fails to
apply" catch a few lines below does reset leader_active+notify+complete-survivors — proving the author knew the hazard but didn't guard these two paths.
Proposed fix: Wrap both sites (or the flushRefBatch body) in catch(...) that completes remaining rt->pending/survivors with the exception, resets leader_active, notifies cve existing careful branch.

5. CAS disk startup probe bypasses skip_access_check, and one unreachable pool aborts DiskSelector::initialize() for the whole server
Risk score: 72 · Source: compatibility · Confidence: high
Files/lines: CasStore.cpp:213-236 (runCapabilityProbe, fail-closed); ContentAddressedMetadataStorage.cpp:startup(); IDisk.cpp:217-238 (probe runs in startupImpl(), before teckAccess()); DiskSelector.cpp:92-139 (no per-disk exception isolation).
Evidence: Store::open unconditionally runs the write/delete capability probe for any writable CAS disk; it throws on failure, inside startupImpl() — which skip_access_check::initialize() has no per-disk try/catch, so one bad CAS disk (mistyped bucket, transient DNS, stale creds at boot) can unwind disk-selector init for every disk, including
unrelated non-CAS tables. The only escape is <readonly>1</readonly>, which also disables all CAS writes.
Proposed fix: Gate runCapabilityProbe on skip_access_check like checkAccess(); independently, isolate per-disk failures in DiskSelector::initialize().

6. Non-Atomic (Ordinary-engine) detached parts route into a spurious sibling namespace that DROP TABLE never cleans up → unbounded blob leak
Risk score: 60 · Source: code-quality-integration · Confidence: high (reviewer verified via extracted+compiled parser)
Files/lines: PartPathParser.cpp:102-125 (findPartDirComponent), consumed by ContentAddressedMetadataStorage.cpp:591-603 (route).
Evidence: For an Ordinary DB (no UUID anchor), the rightmost part-dir-shaped component wins, so detached/attaching_all_0_0_0/... parses to table_uuid="data/db/tbl/detached"0_0" — a namespace distinct from the real table's. Round-trip DETACH/ATTACH works (self-consistent), but DROP TABLE only drops …/tbl@cas@, never …/tbl/detached@cas@; any
part detached at drop time is a permanently orphaned live ref keeping its blobs reachable forever.
Proposed fix: In the non-Atomic fallback, special-case kDetachedDirName — anchor on detached when scanning right-to-left. (Ordinary engine is deprecated; still supported, s

7. SYSTEM CONTENT ADDRESSED GC RUN builds a fresh Cas::Gc per call — can't recover a dead-incumbent lease, and runs unsynchronized against the background loop
Risk score: 55 · Sources: concurrency, code-quality-integration, tests · Confidence: high
Files/lines: CasGcScheduler.cpp:170-174 (runOneRoundNow) vs :176-244 (loop), CasGc.h:118-119,342-358.
Evidence: loop() keeps one Gc instance for the thread's life specifically because the lease-steal protocol needs a stable observer across consecutive rounds. runOneRoundNowc(store, gc_id) every call, so its has_observation/last_seen_owner reset each time — re-issuing the SQL command can never accumulate the two observations needed to steal astuck lease from a dead peer's gc_id. Separately, runOneRoundNow takes no lock against the live loop() thread, so a manual round and a scheduled round run concurrently under the same gc_id — the class comment calls this unsupported ("duplicate ids make two leaders indistinguishable"). CAS-token safety likely caps damage to duplicated work, but it's a documented-contract
violation reachable by a normal admin action.
Proposed fix: Reuse a single persistent Gc instance for both loop() and runOneRoundNow, and serialize manual rounds against the background round under mutex.

8. SYSTEM CONTENT ADDRESSED GC REBUILD FORCE with no disk broadcasts the safety-guard bypass to every CA disk; one access type covers routine GC and disaster-recovery rebui
Risk score: 55 · Sources: ux, security · Confidence: high
Files/lines: InterpreterSystemQuery.cpp (runContentAddressedGcRebuild loops getDisksMap() when disk name empty), AccessType.h:351 (single SYSTEM_CONTENT_ADDRESSED_GARBAGE_Cs).
Evidence: REBUILD FORCE bypasses the "healthy-state" refusal and discards live GC bookkeeping (fresh generation, reset fold baseline). With the disk omitted (a plausible hat fans out to every CA disk, forcing every healthy pool into an unplanned full rediscovery. And granting a monitoring role "kick off routine GC" silently also grants
"force-rebuild every pool's GC state."
Proposed fix: Require an explicit disk (or ALL) for FORCE; split into a distinct SYSTEM_CONTENT_ADDRESSED_GC_REBUILD access type.

9. CAS read path re-parses the full part path on every IMetadataStorage call; getView hit path takes a per-disk global mutex + 2 allocations for a debug-only journal
Risk score: 60 · Source: performance · Confidence: high
Files/lines: PartPathParser.cpp:6-26,129-139, ContentAddressedMetadataStorage.cpp:562-609 (parse+route, no memoization); CachedPartFolderAccess.h:124-128 + .cpp:310-324 (exen unconditionally on every read; cacheKey() allocated twice per getView).
Evidence: Every existsFile/getFileSize/getStorageObjects/… independently runs parsePartFilePath (char-by-char split with a push_back copy) + route (~10-15 small allocations), several times per file-open — vs 1-2 allocations for non-CAS metadata backends. getView's cache-hit path additionally takes the per-table state_mutex and the per-disk explain_mutex (labeled
"Test/log-only decision journal") on every file-open of every thread, plus two cacheKey() heap allocations. On wide-table/high-concurrency CAS SELECTs this is real serializent.
Proposed fix: Resolve the route once per file-open and pass it down (or memoize per raw path); make the explain journal opt-in behind a setting, disabled by default; computin splitNonEmpty.

10. Expect: 100-continue is injected by default for any ≥1 MiB conditional S3 PUT — including the pre-existing non-CAS Iceberg conditional-commit path — with no CAS gate an
Risk score: 52 · Sources: compatibility, performance, tests · Confidence: medium
Files/lines: PocoHTTPClient.cpp:623-683; S3Defines.h:36-40 (DEFAULT_EXPECT_CONTINUE_MIN_BYTES=1 MiB); Iceberg Utils.cpp (unmodified, uses object_storage_write_if_none_match
Evidence: makeRequestInternalImpl is the shared path for all S3 traffic; the new logic sets Expect: 100-continue + blocks on peekResponse for any conditional PUT ≥1 MiB (deeberg metadata/manifest-list commits already use If-None-Match and can exceed 1 MiB, so an S3-compatible store or proxy with weak 100-continue handling could see added
latency/spurious failures after upgrade. Zero test coverage at any level (this fix exists because of a real ~40-min production stall, B118).
Proposed fix: Scope to CAS-owned writes via a WriteSettings flag (as done for s3_force_single_part_upload), or document the expect_continue_min_bytes default in release notes; add a mock-HTTP-server test for the 100-continue / body-skip branches.

11. The two GC CurrentMetrics gauges are process-global — silently clobbered with ≥2 CAS disks on one server; no /metrics-reachable "GC stuck" signal
Risk score: 52 · Source: operability · Confidence: high
Files/lines: CurrentMetrics.cpp:231-232 (CasGcIsLeader, CasGcPendingReclaimEntries); CasGcScheduler.cpp:135-138.
Evidence: Each CA disk gets its own CasGcScheduler, but all write the same flat atomics (no per-disk label). A server with a hot+cold CA tier scrapes one value reflecting wt — hiding a stuck GC on one pool behind the other. There's no AsynchronousMetric for "seconds since last successful round," so a standard Prometheus alert can't be built
from the metrics endpoint; only the async-flushed, per-server content_addressed_garbage_collection_log has it. Ref-append-lane "wedge" state (finding 4) is queryable only v
Proposed fix: Move these to a per-disk system.content_addressed_mounts-style row, or add per-disk AsynchronousMetrics (last-success age, is-leader, pending-reclaim, wedged-

12. The load-bearing EDGE-BEFORE-OBSERVE ordering invariant is guarded only by chassert(precommitted), compiled out in release
Risk score: 55 · Source: code-quality-core · Confidence: high
Files/lines: CasBuild.cpp:247,270 (observeAndAdmit); doc CasBuild.h:62-70.
Evidence: The comment states the ADOPT path is "safe ONLY under this build's durable precommit closure — asserted by chassert(precommitted)." chassert is a no-op in release that calls putBlob/observeAndAdmit before precommitAdd would silently adopt an existing incarnation without watermark protection → a later dangling-reference/data-loss
incident with no production signal.
Proposed fix: Promote to a real throw (LOGICAL_ERROR) in the ADOPT branch. (Matches the project's own recurring "chassert isn't a release fail-close" blind spot.)

13. Architecture: MergeTree part-path parsing leaked into generic DiskObjectStorageTransaction; Cas::Store is an 8-responsibility god class; the sanctioned facade seam is ust sites
Risk score: 55 · Source: architecture · Confidence: high
Files/lines: DiskObjectStorageTransaction.cpp:1-4,69-76,175,200 (includes PartPathParser.h, calls ContentAddressed::parsePartFilePath/isMutablePerPartFile inside moveFile/r19 (Store owns pool lifecycle, build watermark, write-fence, mount-lease + remount thread, ref-append lane, snapshot publish, 2 byte-weighted caches, guarded by 7 mutexes,
friend class Build/Gc); DataPartsExchange.cpp:105-112 (correct cast-to-IContentAddressedExchange) vs DiskObjectStorageTransaction.cpp:343, InterpreterSystemQuery.cpp:2177,2ageSystemContentAddressedMounts.cpp:105 (all cast to the concrete class).
Evidence: The generic disk-transaction layer now embeds MergeTree "mutable per-part file" semantics as an if-branch instead of an IMetadataTransaction virtual. The branch d (narrow interface + cast-to-interface) but applied it once. Store's five independently-reasoned subsystems share one class + friend access, concentrating review/test
burden.
Proposed fix: Push eager-dispatch behind an IMetadataTransaction::requiresEagerDispatch(from,to) virtual so DiskObjectStorageTransaction drops the ContentAddressed::/PartPathree straggler cast sites through a narrow admin facade; extract the remount-thread/caches/ref-append-lane out of Store.

14. Test coverage gaps on the highest-risk, least-soaked paths
Risk score: 55 · Source: tests · Confidence: high
- The conditional-write contract suite never drives Mode::Native (real S3/GCS wire semantics: 412/404/409 taxonomy, ETag/If-Match, ListObjects pagination) — only the in-prokend_contract.cpp:249-257). The entire correctness model rests on emulation matching the wire.
- The DiskObjectStorageTransaction CA dispatch/ordering logic (the B182 fix) has zero direct coverage — every CA test calls one layer below it.
- Concurrency invariants (single-writer slot, dropNamespace-cancels-builds, GC leader/lease/heartbeat, ack-floor) are validated almost entirely by sequential-logic tests; ton. Only monotonic snapshot adoption has a real-thread test.
- Expect:100-continue (finding 10), copyObjectConditional fail-close (S3ObjectStorage.cpp:751-810), and LocalObjectStorage TOCTOU-hardened listObjects (:427-495) have no te.
Proposed fix: Add a Mode::Native contract row (env-gated / with_rustfs); a DiskObjectStorageTransaction-over-CA ordering test reproducing B182; a shared fault-injection bacng tests for each concurrency claim.

Minor issues / improvements

- Late Predecessor PUT (deep-audit, risk 55, self-acknowledged in spec): a fenced-out predecessor's ref-log PUT can land after the successor's startup LIST and be lost if ahe spec-required diagnostic counter/fault-injection hook appears unimplemented — add ProfileEvents::CasRefLatePredecessorObserved so the residual risk is at least
measurable. CasStore.cpp:1690-1706.
- copyS3File throw sites lost message_format_string (compat, risk 22): rewritten to S3Exception(fmt::format(...), …) instead of PreformattedMessage::create(...), degrading  grouping for all S3 copy/backup errors. copyS3File.cpp:68-74,126-138 — match WriteBufferFromS3.cpp:707-711.
- removeFileIfExists (deferred) + mutable writeFile (eager) can execute out of program order (code-quality-integration, risk 40, dead code today): silently drops the write rsion is the repro but has no callers. Give unlinkFile/removeFileIfExists the same CA-eager dispatch. DiskObjectStorageTransaction.cpp:274-280,
ContentAddressedTransaction.cpp:1320-1336.
- dropRefBestEffort swallows rollback failure with no logging (lifetime, risk 42): a correlated backend outage during partial-commit rollback can leave a permanently-live phantom ref (GC reclaims only unreferenced objects, so it won't clean it up). Add tryLogCurrentException. CachedPartFolderAccess.cpp:283-296.
- MultipleDisksObjectStorageTransaction + shared_from_this() → bad_weak_ptr (lifetime, risk 35, latent): double enable_shared_from_this base means the CA writeFile pinning subclass; not reachable today (cross-disk CA copy hits notYet first). Drop the redundant base. DiskObjectStorageTransaction.h:23,140.
- system.content_addressed_mounts column types (ux, risk 30): server_uuid exposed as hex String (other tables use DataTypeUUID); started_at_ms/expires_at_ms as raw UInt64 e)) — the sibling logs in this same branch got it right.
- Enum decoded without range check in CasGenerationSeal (code-quality-core, risk 28): RefNsCleanupState/TokenType static_cast from wire bytes with no validation, unlike eve GC decision logic. CasGenerationSeal.cpp:122,140.
- Two new system logs ship enabled-by-default yet SystemLog.h:20-21 documents content_addressed_log as "off by default" — reconcile comment vs config.xml.
- server_root_id missing-key error throws a raw Poco NotFoundException instead of a ClickHouse Exception (compare the metadata_type check 3 lines up). MetadataStorageFactor
- Stale notYet message points at cache-over-CA as "not supported yet," but that was fixed earlier in this range. ContentAddressedTransaction.cpp:61-71.
- cas_-prefix inconsistency across ~4 of ~20 disk config keys; the command had no verb at all (a bare `GARBAGE COLLECTION` noun phrase) — fixed by the later F1 rename to `SYSTEM CONTENT ADDRESSED GC RUN`.
- CAS relink advertises the wrong disk's pool UUID in a tiered CA+non-CA policy → LOGICAL_ERROR instead of graceful byte-fetch fallback (code-quality-integration, risk 55,  to be a supported config). DataPartsExchange.cpp:534-551,715-718.
- CasLayout.h is 527 lines fully inline across 27 TUs — move the non-trivial parsers to a .cpp (header-hygiene, contained to the CAS module; no shared-header leakage found  clean).

Needs verification

- Whether mixed CA + non-CA (tiered hot/cold) storage policies are a supported/tested topology — governs severity of the relink pool-UUID mis-advertise (finding, minor list Iceberg exposure.
- Live confirmation that an unreachable CAS bucket at boot actually aborts whole-server disk-selector init (finding 5) — traced in code, not run.
- Expect: 100-continue behavior against real AWS S3, MinIO, Ceph RGW, and transparent HTTP proxies (only RustFS/GCS validated in comments).
- Blast radius of two concurrent GC rounds sharing one gc_id (finding 7) — needs a Core-team answer or a targeted concurrency test; likely redundant-but-safe given CAS toke
- Whether any CA gtest scratch-dir name collisions matter — depends on whether CI runs unit_tests_dbms as >1 concurrent OS process.

Suggested commit / diff split (for upstreaming)

This cannot land as one PR (1833 commits). Natural, independently-buildable boundaries:
1. Drop poc/cas_mergetree/ (orphaned spike, 1463 lines, fully superseded, generic class names GC/Engine/Catalog pollute symbol search) — not part of any split.
2. Prune 14 never-incremented ProfileEvents (leftovers of the removed shard-mutation-queue and pre-incarnation GC design) before they become an implicit system.events contr1.
3. GCS conditional dialect + GOOG4 signer (self-contained).
4. Generic conditional-S3-IO enablement (WriteBufferFromS3/copyS3File/S3ObjectStorage conditional writes, Expect:100-continue) — usable independent of CAS.
5. Narrow interface extensions (IObjectStorage/IMetadataStorage/IDiskTransaction) — small, default-safe.
6. CAS core engine → integration layer → SQL/ops surface.

Tests to add or strengthen

- Mode::Native contract-suite row (real 412/404/409, ETag, list pagination).
- DiskObjectStorageTransaction-over-CA ordering test reproducing B182 (mutable-file rename + moveDirectory in one transaction).
- Real-thread interleaving tests: dropNamespace racing an in-flight build; two Gc instances racing a lease steal; flushRefBatch resolveByExactGet-throw with a second caller queued (finding 4); ~Store() racing scheduleRemount() (finding 1).
- Corruption tests for CasRunFile (inflated rec_count), and future-version rejection for CasManifestCodec.
- Cross-pool / to_detached relink fallback (finding 2), copyObjectConditional fail-close branch, LocalObjectStorage concurrent-deletion walk.

Final verdict

Status: request changes

Minimum required actions before merge / upstreaming:
1. Fix the ~Store() teardown crash (finding 1) — blocker.
2. Close the relink RBAC/confidentiality bypass or gate it off-by-default with documentation (finding 2) — blocker.
3. Harden RunFileReader bounds/CRC (finding 3) and the flushRefBatch wedge (finding 4).
4. Restore the skip_access_check escape hatch for the CAS startup probe (finding 5) and fix the Ordinary-engine detached-part leak (finding 6).
5. Promote the EDGE-BEFORE-OBSERVE chassert to a real check (finding 12).
6. Drop poc/cas_mergetree/ and prune the 14 dead ProfileEvents.
7. Add the missing tests for the Native wire path, the transaction-dispatch ordering, and the concurrency invariants (finding 14).

The performance (findings 9–10), operability (finding 11), GC-command (findings 7–8), and architecture (finding 13) items are strongly recommended but not merge-blocking.