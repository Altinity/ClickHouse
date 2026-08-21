---
description: 'Live backlog — read/write path performance, insert/write-path optimization candidates, and scalability findings from the full-scale campaign.'
sidebar_label: 'Performance'
sidebar_position: 8
slug: /superpowers/cas/backlog/performance
title: 'CAS Backlog — Performance'
doc_type: 'guide'
---

# CAS Backlog — Performance {#performance}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for read/write-path performance,
insert/write-path optimization candidates, and scalability findings from the full-scale campaign.

## Read / write path {#read-write}

- **[ckpt-read-policy] Modular `_ckpt` first-attempt view: conservative / cached / prefetch** — DESIRABLE — USER-DIRECTED design shape, 2026-08-03: `_ckpt` handling must be modular/replaceable. Protocol-adjacent (touches the commit path); ships as an explicit reviewed decision with before/after numbers, per the `HEAD`-before-`PUT` protocol-step veto.

  Cost being addressed: every committed ref-log chunk pays `GET _ckpt` + token-CAS serially after the log `PUT`, so a lone `INSERT` pays +4 serial RTTs. A pluggable policy chooses only where `publishCkpt`'s first attempt gets its `{body, token}` view — the invariant core (retry-after-conflict always does a whole-body exact re-read, `lifeEpochWouldDecrease` re-checked after any re-read, durability order `log PUT → _ckpt CAS → ack`) is shared and policy-independent.

  Policies: (1) **conservative** = today, fresh GET per publish; (2) **cached** = seed with one GET on first touch, then serve from the writer's own last winning CAS and go straight to PUT-if-match (expected to almost always hit, since lease exclusivity excludes cross-process writers — a miss is a signal, not noise); (3) **prefetch** = one paginated LIST at mount seeds all `_ckpt` views, then memory-only + PUT-if-match (LIST is a pure hint here, correctness still rides the conditional write). Mandatory cache-invalidation edges for (2)/(3): fence-generation change, wedge, remount supersession, `catalog_life_invalidated`. Always-exact-read, out of the policy seam: recovery's `_ckpt` sample and GC-fold's frontier GET.

  Effect: +4 → +2 RTTs per lone insert. MEASUREMENT PRECONDITION: the stage-1 1.59x figure predates `_ckpt` (measured before it landed) — re-run the wide-insert baseline on current HEAD before benching policies against it.
- **[write-path stage 1] parallel intra-part blob upload — LANDED (2026-07-24)** — Fanned out a part's blob PUTs/dedup-HEADs (`CasBlobUploadPool`/`fanOutBlobUploads`): CA wide-insert wall 58.41s → 30.26s, CA-vs-plain 3.0x → 1.59x. Residual gap = the serial cross-part commit (stage 2, `{#stage2-concurrent-commitpart-postponed}`, POSTPONED by user decision) plus the CAS-only dedup HEAD/GET traffic (`[B121 / B202 / one-GET-open]` below).
- **[TXN-ONE-PIPELINE] complete the "staging ops never defer" invariant** — HARD (small, structural) — `DiskObjectStorageTransaction`'s two dispatch pipelines (eager staging ops vs. deferred-to-commit durable ops) caused the `01603` column-TTL abort ordering inversion; the correct invariant is per-state-domain, not a total order. Target shape approved by the user and superseding earlier staged-intents wording: an everything-immediate model with a single `dispatch` funnel (no CA subclass), a two-phase `IDiskTransaction::precommit()`/`commit()` contract (CA precommit = the entire publish; CA commit = durable-intent materialization only), `commit` implicitly running `precommit` when not called (with `CasImplicitPrecommitInCommit` observability), plus a de-patching pass removing accumulated eager-dispatch/read-your-writes workarounds from non-CA files (`docs/superpowers/cas/upstream-patch-inventory.md`). Lands before codecs v3 and the source-layout refactoring.
- **[B121 / B202 / one-GET-open] read request-count reduction** — DESIRABLE (design pass) — B202 inline-by-size (drop the file-type predicate, inline < ~512 KiB, weigh the wide-part-medium-column regression, `.bin` carve-out) + a per-blob-GET read-cost reduction (B121) + one-GET part open (pack small files). Pure perf/request-count; no safety dimension. Companion to the (landed, opt-in) file-cache disk for re-read-heavy workloads. (An orphaned 2026-08-04-triage finding covers the same class via a measured DownloadPart/relink-fetch dominant read cost — folded in as confirmation.)
- **[B98] Streaming `putOverwrite` (condemned-displacement)** — DESIRABLE — The rare INV-1 revival/displacement path still materializes the whole body; not a blocker.
- **[promote-recreate] promote-time in-place recreate of a condemned SOURCED (tokened) blob** — DESIRABLE — The tokened promote gate stays fail-closed `ABORTED`; recreate happens on the retried build via `putBlob` cold-reuse. The tokenless-evidence copy-forward case is DONE. Ideal root-cause fix (writer-triggered synchronous fold-barrier at promote) is blocked by the lack of a writer↔GC synchronous-fold API — deferred behind the landed bounded resurrect.
- **[R1/X1] ephemeral reader pin (cross-node GC fence)** — DESIRABLE / VERIFY — Per-server-owned namespaces narrow the window and a live ref resolving to an absent object surfaces `FILE_DOESNT_EXIST` (INV-NO-DANGLE), so for normal MergeTree this is covered by DataPart lifetime; the ephemeral-pin mechanism is design-only. Audit whether any ref-less/cross-node reader path exists before implementing.
- **[ch128ctx] slot-bound blob-hash middle tier** — DESIRABLE (small spec) — New `BlobHashAlgo` variant: `cityHash128(content) ∥ xxh3_64(part_name, file_name) ∥ size` (256-bit; variable-width `BlobDigest` already supports it). Binds blob identity to its minting slot, so *cross-slot* collisions (the realistic adversarial dedup vector: attacker-crafted content deduped into a victim's future blob) become useless, at ~zero CPU cost over `cityHash128`. Every load-bearing dedup survives: relink/carry-forward are reference-based; retry idempotency, same-name replica writes, and snapshot-upload→TTL-move prepayment are same-slot; only cross-slot content coincidence is lost (an explicit non-goal, `01 §what-it-does-not-buy`). Middle tier of `cityHash128` → `ch128ctx` → `sha256`. Main touch: the hasher interface needs `(part_name, file_name)` context injection into `putBlob`. Origin: backup manifest-reuse discussion, `10-backups.md §multi-disk` (2026-07-14).
- **[codex-26] `casAppendObject` needed before any concurrent appender** — LOW (latent) — `CasPlainObjects::casPutObject`'s CAS loop re-reads only the TOKEN on conflict, retrying with the SAME frozen `bytes` payload the caller froze at buffer-open (`ContentAddressedTransaction::writeFile`'s Append branch) — a fresh-token/stale-payload lost-update shape (2026-07-17 codex-review triage, finding №26). Not reachable today: the only production appender is the mutation-entry CSN write (`MergeTreeMutationEntry::writeCSN`), one append per mutation-unique key under the per-table single-writer lease, so there is no second appender to lose. Required before any future concurrent appender lands: a real `casAppendObject` that re-reads the base content (not just the token) inside the retry loop. See the single-appender invariant comment at `casPutObject`.

## Write-path optimization candidates after stage 1 (2026-07-24) {#writepath-candidates-post-stage1}

Context: stage 1 (parallel intra-part blob upload) took the wide 10M×30col×500part CA-S3 `INSERT` from
58.41 s to 30.26 s (3.0× → 1.59× vs plain S3); the workload is still ~87% network-bound. Reports:
`docs/superpowers/reports/2026-07-23-cas-wide-insert-baseline.md` (baseline),
`docs/superpowers/reports/2026-07-24-cas-wide-insert-stage1-effect.md` (stage-1 effect). The residual
splits between the serial cross-part commit (stage 2's target, program point 7 — active, NOT a backlog
item) and the items below. STANDING USER VETO: the `HEAD`-before-`PUT` dedup gate (~12% of wall,
268.8 `HEAD`/part) and any change to the durable-op protocol are NOT candidates.

- (1) **Enable S3-native staging on the wide-insert profile and measure** — the feature exists
  (opt-in, write-once conditional server-side copy; validated e2e). Local staging then upload moves
  every blob's bytes twice; native staging may cut wall on S3 backends. Zero new code: flip the
  setting in an s41 variant leg and compare. Status: MEASURE.
- (2) **S3 client concurrency/connection tuning for the upload pool** — with 16-33 threads now
  issuing PUTs concurrently, client-side limits (connections, per-request concurrency) may cap
  overlap. Config-level experiment on s41. Status: MEASURE.
- (3) **Inline-placement threshold tuning** — small part files inline into the manifest
  (`CaInlinePlacement` machinery). The wide profile pays ~239 `PUT`/part (~8 objects/column);
  raising the inline threshold could fold the small tail (marks, minor streams) into the manifest.
  First verify the threshold is a setting (not a pinned format constant), then measure PUT-count and
  wall deltas on s41. Status: INVESTIGATE THEN MEASURE.
- (4) **Repoint waste on part removal** — known class (`project_part_removal_repoint_waste`):
  repoints against `delete_tmp_*` refs ≈ 22% of the writer `PUT` class. Eliminating them changes
  WHICH ledger ops are issued — protocol-adjacent, needs an explicit user decision with a risk
  analysis before any work. Status: DECISION NEEDED (present risk analysis to user).
- (5) **Unconditional manifest `GET` on promote** — part of the 108.7 `GET`/part during insert;
  separate long-standing item. Verification semantics of the write path → under the spirit of the
  protocol veto; do not touch without an explicit user go-ahead. Status: DECISION NEEDED (present
  risk analysis to user).

## Stage 2 (concurrent commitPart) — research notes; POSTPONED by user decision (2026-07-24) {#stage2-concurrent-commitpart-postponed}

USER DECISION: postponed — "слишком сильное / малопредсказуемое влияние на upstream / generic code". Recorded
here so the research is not lost; revisit only with an explicit user go-ahead.

Three orphaned 2026-08-04-triage findings describe the same postponed parallel-write-path design
(a `cas_commit_concurrency`-sized worker pool, a `takeTransactionForBatchCommit` seam, and index-addressable
outcome slots for bounded worker loops) — folded in here as detail on the same postponed design, not new work.

Motivation (measured): after stage 1 the wide CA-S3 `INSERT` residual is 1.59× vs plain S3, dominated by the
serial cross-part commit (`ReplicatedMergeTreeSink::finishDelayed` iterates partitions one at a time; ref-ledger
batch size = exactly 1.0, so per-part manifest/ledger round-trips never batch). Stage 2 = bounded concurrent
dispatch of the per-partition commit; the CAS ledger then batches emergently and blobs multiplex on the stage-1
pool.

Agreed scoping (before postponement): (a) start with `ReplicatedMergeTreeSink` ONLY (the measured path);
(b) then re-run s41 with a non-replicated leg; (c) then the `MergeTreeSink` counterpart as a separate follow-up.

Path anatomy + hazard inventory (from code reading, 2026-07-24):
- Replicated loop body per partition: `finalize` → dedup hashes/block-ids → `commitPart` (Keeper block-number
  alloc → `renameParts` disk txn [the whole CAS write path lives here] → Keeper multi ~`:995-1011` → rollback
  machinery) → dedup-conflict retry loop (`deduplicateBlock` filters the block, then `writeNewTempPart`
  RE-SERIALIZES AND RE-UPLOADS the part, then retries commit) → `resolveQuorum` WAITS inside the iteration →
  `PartLog::addNewPart`.
- Concurrency hazards found: `deduplication_async_inserts_cache_version = 0` reset per iteration is a SHARED
  member (`ReplicatedMergeTreeSink.cpp:455`) — race under fan-out, must become per-task; shared Keeper session
  via `ZooKeeperWithFaultInjection` (raw client is thread-safe; the fault-injection wrapper needs verification);
  shared caches `deduplication_hashes_cache` / `async_block_ids_cache` `triggerCacheUpdate` from multiple
  threads needs verification; quorum ordering semantics change (today partition N+1 does not commit until N's
  quorum resolves) — recommendation was to force serial when quorum is enabled; a FULL shared-state inventory
  of `commitPart` (storage counters, rollback checkpoints) was identified as the main design work and was NOT
  completed.
- Plain `MergeTreeSink::finishDelayedChunk`: simpler loop (finalize → `deduplication_log->addPart` →
  `renameTempPartAndAdd` → PartLog); hazards: non-replicated dedup-log append concurrency, too-many-parts
  delays. Unmeasured (s41 is Replicated).
- Patch shape (approach 1 of 3, recommended at the time): private `processDelayedPartition(partition)` +
  bounded `ThreadPoolCallbackRunnerLocal` fan-out + setting `max_concurrent_part_commits_per_insert`
  DEFAULT 1 (feature dormant = today's serial behavior; minimal fork-rebase risk), all-drain + first-error,
  per-task `ProfileEventsScope`, B90 capture discipline. Estimated diff ~100-150 lines. Rejected alternatives:
  commit-only fan-out with caller-side retry queue (async state machine, NOT compact); window-2 pipeline
  (complexity without the win).
- Expected effect calibration: even a perfect stage 2 does not reach 1.0× — ~12% of wall is the vetoed
  `HEAD`-before-`PUT` dedup cost; realistic target ~1.2×.

## Write-path allocation and ref-table commit-path cost (2026-07-16, TXN-Final campaign) {#writepath-cost-txn-final}

- **[write-path-alloc-audit] CA write-path allocation / memory audit** — During the TXN-Final full CA-default stateless run, `system.trace_log` showed the CA write path dominates the Memory (allocation-sampling) trace: `ContentAddressedTransaction::tryCreateWriteBuffer` (~489k samples) + `writeFile` (~488k), then `CaInlineWriteBuffer` (~322k) and `CaContentWriteBuffer` (~165k). CPU was clean (NO CAS symbol in the top-15 CPU stacks) — so this is NOT a CPU or correctness issue, purely an allocation-volume observation. TODO — a deliberate alloc-profile pass on the CA write path: is `tryCreateWriteBuffer` allocating more than necessary per file (write buffer + `std::function` finalize closure + captured `owner shared_ptr<IDiskTransaction>`)? Confirm `CaInlineWriteBuffer` isn't growing its buffer inefficiently; `Cas::ObjectStorageBackend::emuWrite`/`putIfAbsent` take header maps BY VALUE (pass by const-ref, emulated test path only); establish a pre-/post-TXN-ONE-PIPELINE baseline to confirm the write-hook refactor didn't add per-write allocation overhead. Not correctness-blocking.
- **[ref-table-copy-commit-path] CA ref-table copy on the commit/ref-op path** — The #1 CPU stack in the TXN-Final soak (pure-CA workload) was deep copy-construct + destroy of the whole committed-ref map (`RefTableState`) via `Cas::Store::appendRefOps`/`flushRefBatch` → `ContentAddressedTransaction::commit` → `publishStaging`. Overall CPU is low (I/O-bound), so not a current hog, but a scalability smell: every ref op on the commit path appears to copy the entire ref-table state by value, cost growing ~O(refs) per commit, ~O(refs·commits) over a workload — compounds under insert/mutation-heavy loads. TODO (perf, likely pre-existing ref machinery, not a TXN regression): investigate whether the `RefTableState` snapshot in `appendRefOps`/`flushRefBatch` can be passed by const-ref / diffed incrementally / copy-on-write instead of full-copied per ref batch; confirm pre-existing via git blame; alloc-profile the commit path together with the write-buffer note above. Not correctness-blocking (soak green, dangling=0).

## The pool-wide catalog is a write hot spot, measured on the CA-s3 lane {#ref-catalog-write-hotspot}

Measured 2026-07-31: every table creation in a pool writes the same catalog object
(`cas/ref_catalog`), so a lane creating thousands of tables serializes them all through one CAS loop
— 137/250 S3 timeout lines on the CA-s3 stateless lane named this one key. Not evidence the catalog
design is wrong (it exists because pool `LIST` is unreliable), and the measurement is from a lane
that creates tables far faster than any real deployment — but a genuine cost the design didn't
expose before. **Open questions before a fix**: does the write rate come from creation only, or also
from read-mints; is the retry deadline just too short for the contention it now sees; can the object
be sharded/batched without giving up the single-object atomicity the GC universe snapshot needs.

## Scalability findings from the full-scale campaign (S3 budget) {#scale-findings}

These are real scale/budget findings; most are variants of "O(N) GC / per-op amplification". Track for the capacity model + a future S3-budget push.

- **[idle-scratch-debris] idle GC leaves scratch files uncollected on an empty pool** — MINOR — S23 (2026-07-18 secondary finding): `scratch_bytes` on local staging grew 1→21 MiB over an idle window with ZERO inserts and an empty pool — idle GC rounds appear to create scratch files and not clean them. Local-disk debris, not tracked memory; needs its own check + cleanup path look.
- **[scratch=full-part] CAS write spills the whole object to local scratch for hash-before-upload** — DESIRABLE — 100 GiB merge → 93 GiB scratch; a part larger than local free scratch cannot be written. Largely addressed by the (opt-in) S3-native staging; make the local path stream-hash too, or document the staging requirement for very large parts. (An orphaned 2026-08-04-triage finding covers the same cas_scratch spill class, citable across 3 sources — folded in as confirmation.)
- **[replicated double-spill] shared-pool replica re-merges + re-spills its own full scratch** — DESIRABLE — A replica could adopt the leader's uploaded blob instead of re-merging locally (186 GiB scratch for one deduped 100 GiB blob). (An orphaned 2026-08-04-triage finding covers the same shared-pool `OPTIMIZE FINAL` re-merge/re-spill class — folded in as confirmation.)
- **[wide-part O(columns)] merge issues O(columns) S3 ops → ephemeral TCP port exhaustion** — DESIRABLE — S07 20000-col `OPTIMIZE FINAL` stalled in an S3 retry storm. (An orphaned 2026-08-04-triage finding covers the same S07 20000-column finding verbatim — folded in as confirmation.)
- **[partitioned-INSERT O(partitions)] O(partitions) CAS commits per insert** — DESIRABLE — ~10s per 256-partition insert.
- **[startup O(refs)] server startup S3-op cost scales with #tables/refs** — WATCH — ~152k S3 ops to start a 10k-table server (LISTs/GETs, not blob enumeration); recovery still fast.
- **[S11 capacity] deferred-GC disk accumulation under delete-churn** — WATCH — GC does not reclaim during the delete phase (interval-driven); same O(N)-GC-lag family.
- **[Capacity model] GC cadence + snapshot size under typical load** — DOC/DESIRABLE — Estimate GC frequency + per-shard in-degree run / fold-seal sizes at typical production load; validate against a soak's GC log; feeds the `gc_interval_sec` default and trim gates. Live-AWS data point: a round is 30–40s.
- **[physical-footprint amplification]** — VERIFY — 1h soak: `pool_bytes` ~400× `logical_bytes` (rustfs#3231 overwrite-version retention vs CA debris); should collapse under full GC / a compacting store. Not a safety issue (dangling=0).

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[putblob-uncertainty-exhaustion-abort] uncertainty-exhaustion abort in `putBlob`'s 8-round loop crossing the controller budget** — DESIRABLE — A real timeout-class risk: the 8-round bounded retry loop can exhaust against `CasRequestController`'s budget under sustained ambiguity.
- **[manifest-trust-promote-path] manifest-trust promote path: skip per-leaf `HEAD`/`loadMeta` on adopted leaves** — DESIRABLE — A real perf-lever proposal, not yet confirmed landed; verify against HEAD before scheduling.
- **[cas-commit-pool-anti-deadlock] CAS commit pool needs bounded worker-loop callbacks or a dedicated pool sized by `cas_commit_concurrency` to avoid deadlock** — DESIRABLE — A concrete anti-deadlock design requirement, plausibly still needed for the parallel-write-path work.
- **[hot-part-blob-trickle-warmer] optional age-based trickle warmer for hot-part blobs ahead of snapshot** — DESIRABLE — Speculative but well-motivated perf idea (young-merge-window reasoning); borderline vs. not-tracked but the driver is concrete.
- **[ca-trycommit-retry-loses-staged-state] `tryCommit` retry can drop staged `writeFile`/`createHardLink` state (B82)** — DESIRABLE — A CA `tryCommit` retry can lose staged state because a reset `metadata_transaction` has no `operations_to_execute` entry to refill in-memory staging maps. Has a bug number already assigned; verify still reproducible against HEAD and file properly if so.
- **[cache-get-head-token-mismatch] `readManifest`/`get`/`getStream` can cache bytes fetched at one incarnation token under an earlier `HEAD`'s token** — DESIRABLE (correctness) — Independently confirmed still open during the phase-2/3 verdict audit of the docs-consolidation effort itself.

## Suffix allowlist buffers big index files whole in memory (2031-triage CAS-014) {#part-file-suffix-allowlist-memory}

`partFileMustStayBlob` (`ContentAddressedTransaction.cpp:65-71`) is a closed suffix allowlist that
decides streaming-blob vs whole-file-in-memory buffering. It does not know `primary.cidx` (the
DEFAULT name — `compress_primary_key=true` makes the listed `primary.idx` branch dead code),
`.cmrk4` (default `write_marks_for_substreams_in_compact_parts=true`), or any skip-index data file
(`.idx`, `.pst.idx`, `minmax_*.idx`). Those go through `CaInlineWriteBuffer`, which accumulates the
whole payload into a `std::string` and only at finalize applies `INLINE_CAP = 1 MiB` (`:98`, `:932`),
spilling oversized bodies to a blob.

So there is no correctness or manifest-bloat defect (the cap holds), but a vector/text index or a
large primary key is buffered entirely in memory and then written twice. Fix: add the shipped
default names to the allowlist, and — the structural half — emit a log/metric when an unknown
extension takes the buffered path, so the next new MergeTree file name shows up instead of silently
costing memory. Predicate unchanged since `c623713479f`.

## The part-folder view cache byte budget is inoperative (2031-triage CAS-045) {#part-folder-cache-weight-always-256}

`PartFolderView::estimatedBytes` returns `256 + manifest_size` (`Parts/PartFolderAccess.cpp:136-140`)
and `manifest_size` comes from `Cas::Resolved` (`Pool/CasRefProtocol.h:126`), which BOTH producers
hardwire to zero: `CasRefLedger::resolveRef` (`Pool/CasRefLedger.cpp:341-345`) and
`CasRefLedger::listRefs` (`:373-377`). The field has never been populated since it was introduced in
`7a640e5ac69`, so every retained view weighs exactly 256 bytes.

Two consequences, both confirmed at HEAD:

- `part_folder_cache_bytes` (default 64 MiB, `ContentAddressedSettings.cpp:86`) is not a byte budget
  at all — it degenerates to an entry cap of `cache_bytes / 256`, i.e. 262144, far above
  `part_folder_cache_max_entries` (default 10000, `:87`). The only live bound is the entry count, and
  what each entry actually pins is the fully decoded `PartManifest` including every `inline_bytes`
  body (up to the 16 MiB aggregate inline cap per part, see
  `formats-and-storage.md`{#manifest-inline-budget-no-spill}). 10000 wide-part views with
  a few hundred KiB of inline `checksums.txt`/`serialization.json`/`primary.cidx` each is gigabytes of
  memory an operator believes is capped at 64 MiB. `CurrentMetrics::CASPartFolderCacheBytes`
  (`CurrentMetrics.cpp:233`) reports the same fiction.
- the oversized-entry bypass at `Parts/PartFolderAccess.cpp:226` compares 256 against
  `part_folder_cache_max_entry_bytes` (default 16 MiB, `ContentAddressedSettings.cpp:88`), so nothing
  is ever excluded for being too large and `CASPartFolderViewOversizedBypasses` can only ever be zero.
  The setting and the metric are both dead.

No correctness impact — this is memory accounting only, and the entry cap keeps the cache finite —
hence P2. Owed: weigh the view from the decoded body it actually owns (sum of entry path lengths plus
`inline_bytes` sizes plus a fixed per-entry overhead) and delete `Resolved::manifest_size`, whose only
consumer is this weight; a `manifest_size` sourced from the ref ledger cannot be made to work because
the ledger never learns the body size. A gtest should assert a manifest with a large inline body
weighs more than an empty one (today `gtest_cas_part_folder_view.cpp:50` passes `manifest_size=1000`
by hand, which is why the unit tests never noticed).

## The DROP/REPLACE PARTITION covering part is published under `DataPartsLock` (2031-triage CAS-048) {#covering-part-publish-under-datapartslock}

P3, narrow. `MergeTreeData::removePartsInRangeFromWorkingSetAndGetPartsToRemoveFromZooKeeper`
(`src/Storages/MergeTree/MergeTreeData.cpp:5842`) receives the caller's `DataPartsLock` and, on a
content-addressed disk, creates AND publishes the empty covering part inside it: `createEmptyPart`
(`:5913`), the tmp→final rename (`:5916`), and the explicit
`getDataPartStorage().commitTransaction()` at `:5937-5939`. On CA that last call is the whole remote
publish — `stageManifest` + `precommitAdd` + blob uploads + `promoteBuild` with a ref-log CAS
(`ContentAddressed/ContentAddressedTransaction.cpp:409-425`) — so the table's exclusive parts lock is
held across several object-store round trips, including a CAS append that retries under writer
contention. Callers that reach it: `StorageReplicatedMergeTree.cpp:2987` (`executeDropRange`), `:9343`
and `:9608` (REPLACE/MOVE PARTITION), and `MergeTreeData.cpp:5766` for plain MergeTree.

Why it is P3 and not a gate: the part is EMPTY (a few small blobs + one manifest + one log append);
the same lock already spans `createEmptyPart`'s object-store writes on an ordinary object-storage
disk, so lock-held remote I/O on this path is inherited upstream behaviour rather than a CA
regression; the trigger is DDL only; and a failure is loud (the exception propagates and the queue
entry retries) with the per-ref rollback in `commit` preventing a silent partial publish.

Why it was not fixed with the off-lock move: `77484196b0d` moved the disk commit off the parts lock in
`Transaction::renameParts` (`MergeTreeData.cpp:8995`), but this path deliberately never reaches
`Transaction::commit` — it rolls the in-memory transaction back so the cover stays `Outdated`
(`:5942`), which is exactly why the hand-placed `commitTransaction` exists (see the comment at
`:5921-5936`). Moving it off-lock requires a three-phase restructuring of the caller (compute the
covering `MergeTreePartInfo` under the lock, release, create+publish, re-acquire and re-validate that
the range is still the one that was computed) — the re-validation is the hard part, so this is a
design task, not a code move.

## Background snapshot-publish fan-out is unbounded pool-wide (2031-triage CAS-051) {#snapshot-publish-fanout-unbounded}

The single-in-flight gate on background snapshot publishes is PER TABLE —
`admitSnapshotPublishUnderStateLock` reads the per-runtime counter (`Pool/CasRefLedger.cpp:3971`, `Pool/CasRefLedger.h:826`) —
and `dispatchSnapshotPublisher` spawns a fresh detached `ThreadFromGlobalPool` per admitted publish
(`Pool/CasRefLedger.cpp:4005-4017`). There is no pool-wide counter, semaphore, or dedicated pool. The
trigger threshold is per namespace and low (`snapshot_log_count_threshold = 256`,
`snapshot_log_bytes_threshold = 1 MiB` — `Pool/CasPool.h:234-235`), so an ingest wave that crosses it on
N tables of one pool starts N concurrent whole-namespace re-encodes plus conditional PUTs at once.

Fail-soft, so not a correctness item: a global-pool exhaustion throw is caught, the pending count is
undone and the publish is retried on the next trigger (`Pool/CasRefLedger.cpp:4019-4032`), and the
per-table backoff (`:4082-4092`, honoured at `:3974`) suppresses PUT storms after a non-durable publish.
The bound is "number of CA tables", not infinity. Owed: a pool-wide limiter on concurrent snapshot
publishes (a configurable max, or a shared `ThreadPool`), keeping the existing per-table single-flight
gate underneath it.

Note for readers of the same finding: the claimed pending-count leak on a failed dispatch (which would
hang the untimed waits at `Pool/CasRefLedger.cpp:1710-1713` and `:5106-5111`) does NOT exist — it was
closed by `829ad698ef6`.

## The ref-table cache budget is enforced only on cache admission, is not operator-tunable, and its running total can underflow (2031-triage CAS-053) {#ref-table-cache-budget-admission-only}

`CasRefLedger::enforceRefTableCacheBudget` (`Pool/CasRefLedger.cpp:1611-1690`) has exactly one call
site: the tail of `ensureRefTableRecovered` (`:1550`), which is reached only when a recovery actually
ran — a warm touch returns early at `:1339`. So the whole-table cache budget
(`ref_table_cache_bytes`, default 256 MiB, `Pool/CasPool.h:265`) is an admission-time check, like an
LRU that only evicts on insert. It is never re-evaluated when the resident tables GROW in place:
`tail_bytes_since_snapshot` is incremented by every applied txn (`:2470`, `:3791`) and
`base_snapshot_bytes` is refreshed on each publish (`:1568`), yet a stable working set that is written
forever and never re-recovers cold triggers no pass at all. A fixed set of hot tables can therefore sit
above the configured ceiling indefinitely.

Three separate residuals, all confirmed at HEAD:

- No re-enforcement on in-place growth (above). Partly inherent: the `use_count() == 1` gate at `:1650`
  and `:1659` is a correctness gate, not a policy choice — it is what makes append-lane split-brain
  impossible, and a table being written is by construction not evictable. But the pass also never runs
  for tables that HAVE gone idle, because nothing but a cold admission calls it.
- `ref_table_cache_bytes` is the only cache budget in `PoolConfig` with no `ContentAddressedSettings`
  entry: `deduplication_cache_bytes` (`ContentAddressedSettings.cpp:70`) and
  `manifest_decode_cache_bytes` (`:90`) are declared settings and wired in
  `ContentAddressedMetadataStorage.cpp:759` and `:761`, while `ref_table_cache_bytes` is set nowhere
  outside gtests — the 256 MiB default is effectively hardcoded in production. There is also no
  `CurrentMetrics` gauge for the ref-table cache (only `ProfileEvents::CASRefTableEvictions`,
  `ProfileEvents.cpp:788`), so an operator can neither size it nor observe it.
- `total -= c.weight` (`Pool/CasRefLedger.cpp:1667`) is an unclamped unsigned subtraction over values
  read in two different passes of the same critical section: `total` sums `weightOf` for every table
  (`:1634-1636`) including hot ones whose atomics are mutated under `state_mutex` only, while `c.weight`
  is captured later (`:1657`). A table that was hot during the `total` loop and idle by the candidate
  loop can contribute a larger weight than it did to `total`; if that increase exceeds every other
  table's weight the subtraction underflows, `total <= budget` stays false and every idle `Ready` table
  is evicted in one pass. `clampedCounterSub` (`:4191-4198`) already exists for exactly this class and
  is used at `:4423-4424`; this site does not use it.

Why P3 and not a gate: no correctness impact. Eviction is gated on idle + `Ready` + non-wedged
(`:1650-1660`), an evicted table is re-recovered from the durable snapshot+log on next touch
(`gtest_cas_ref_writer.cpp:2025` pins this), so the underflow's worst outcome is a burst of extra
recovery I/O, and the missed enforcement only lets resident memory track the working set's real ref-map
size rather than the nominal cap. Owed, in increasing cost: clamp the subtraction; expose the budget as
a `ContentAddressedSettings` entry plus a `CurrentMetrics` gauge; and call the pass from a second,
growth-driven trigger (for example after a snapshot publish updates `base_snapshot_bytes`) so the cap
means something for a long-lived stable table set.

## `createHardLink` pays one manifest `HEAD` per file of the source part (2031-triage CAS-055) {#hardlink-per-file-forcefresh-head}

The committed-source carry-forward branch of `ContentAddressedTransaction::createHardLink` resolves the
source part `ForceFresh` on every call (`ContentAddressedTransaction.cpp:1190`), and with the shipped
`part_folder_validate = always` default (`ContentAddressedSettings.cpp:89`) `ForceFresh` never serves a
retained view (`Parts/PartFolderAccess.cpp:197` gates the short-circuit on
`validate.mode != Always`), so each call reaches `buildView` → `readManifestShared`, whose `HEAD` is
mandatory even on a decode-cache hit (`Pool/CasManifestReader.cpp:63-65`). A `FREEZE`/clone or an
`ALTER ... UPDATE` hardlinks every unchanged file of the part through ONE CA transaction
(`DataPartStorageOnDiskBase.cpp:530-540` self-creates the clone transaction;
`MutateTask.cpp:3445` opens one for the new part), so a wide part costs one `HEAD` per file for work
that copies nothing. The manifest decode is served from cache on a token match, so the audit's "full
view rebuild per file" overstates it — the residual is the `HEAD` round trip, not a re-decode.

The fix already exists one function away and needs no protocol change: `unlinkFile` memoizes the proof
per `(transaction, ref)` in `force_fresh_validated_refs` and downgrades the rest of the burst to
`CachedForLoad` (`ContentAddressedTransaction.cpp:1595-1603`), which still revalidates the manifest id
against a fresh resolve and rebuilds on mismatch (`Parts/PartFolderAccess.cpp:177-186`). Apply the same
memo to the createHardLink committed-source branch. The `Always` default itself stays — it is the
fail-closed policy, and relaxing it is the separate, gated `part_folder_validate` question
(`{#part-folder-validate-never-gating}`).
