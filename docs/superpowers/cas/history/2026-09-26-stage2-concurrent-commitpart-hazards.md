---
description: 'History: full path anatomy and concurrency hazard inventory for the postponed stage-2 concurrent commitPart design, from the 2026-07-24 code-reading pass. Moved out of the live backlog entry to keep it short; the postponement decision and the MUST items it still owes stay in BACKLOG/performance.md.'
sidebar_label: 'Stage 2 concurrent commitPart hazards (history)'
sidebar_position: 2
slug: /superpowers/cas/history/stage2-concurrent-commitpart-hazards
title: 'CAS history: stage 2 concurrent commitPart hazard inventory'
doc_type: 'reference'
---

# CAS history: stage 2 concurrent commitPart hazard inventory {#stage2-concurrent-commitpart-hazards}

Full detail behind `BACKLOG/performance.md`'s
[`{#stage2-concurrent-commitpart-postponed}`](/superpowers/cas/backlog/performance#stage2-concurrent-commitpart-postponed)
entry. The design is POSTPONED by user decision (2026-07-24) — "too strong/unpredictable an effect on
upstream/generic code" — and is recorded here so the research is not lost; revisit only with an explicit
user go-ahead.

Three orphaned 2026-08-04-triage findings describe the same postponed parallel-write-path design
(a `cas_commit_concurrency`-sized worker pool, a `takeTransactionForBatchCommit` seam, and index-addressable
outcome slots for bounded worker loops) — folded in here as detail on the same postponed design, not new work.

Motivation (measured): after stage 1 the wide CA-S3 `INSERT` residual is 1.59x vs plain S3, dominated by the
serial cross-part commit (`ReplicatedMergeTreeSink::finishDelayed` iterates partitions one at a time; ref-ledger
batch size = exactly 1.0, so per-part manifest/ledger round-trips never batch). Stage 2 = bounded concurrent
dispatch of the per-partition commit; the CAS ledger then batches emergently and blobs multiplex on the stage-1
pool.

Agreed scoping (before postponement): (a) start with `ReplicatedMergeTreeSink` ONLY (the measured path);
(b) then re-run s41 with a non-replicated leg; (c) then the `MergeTreeSink` counterpart as a separate follow-up.

## Path anatomy + hazard inventory (from code reading, 2026-07-24) {#path-anatomy-hazard-inventory}

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
- Expected effect calibration from the historical profile: even a perfect stage 2 was not expected to
  reach 1.0x because blob presence checks consumed ~12% of wall. That estimate must be remeasured under
  the current mandatory-`HEAD` protocol; the target-only report above is not a matched delta.

(The "target-only report" referenced in the last bullet is the report cited in `BACKLOG/performance.md`'s
`{#mandatory-blob-head-cost}` entry, not a report in this document.)
