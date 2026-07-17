---
description: 'Proposal: how to prevent, detect, and audit long operations under locks in CA MergeTree — born from the publish-under-data_parts-lock incident that our harness never noticed.'
sidebar_label: 'Concurrency checks improvements'
sidebar_position: 90
slug: /superpowers/cas/concurrency-checks-improvements-proposal
title: 'Concurrency Checks Improvements Proposal'
doc_type: 'reference'
---

# Concurrency Checks Improvements Proposal {#concurrency-checks-improvements-proposal}

Status: PROPOSAL (2026-07-17), awaiting prioritization. Owner context: the
`renameParts` durability fix (spec
`docs/superpowers/specs/2026-07-17-part-durability-before-keeper-commit-design.md`) is in review;
everything below is follow-up work after it lands.

## Motivating incident {#motivating-incident}

While tracing the acked-then-lost `INSERT` data loss we found a second, independent defect: since
[TXN-ONE-PIPELINE] Task 1.1 (`39cf3279652`, 2026-07-16) the entire CA durable publish — blob
uploads to S3 + manifest + ref publish — executed inside `MergeTreeData::Transaction::commit`,
which runs **while holding the `data_parts` lock** (`MergeTreeData.cpp:8782` takes the lock, the
disk-commit loop at `:8797-8799` runs under it). Every concurrent query needing the parts lock on
that table stalled behind S3 round trips. In production this would be a large, visible stall
class. Our harness never noticed.

## Why the harness missed it {#why-the-harness-missed-it}

1. **Tiny exposure window.** Before Task 1.1, B151 published at `renameParts` — off-lock. The
   under-lock publish existed for about one day of soaks (one 20-minute soak, one stateless
   sweep, R4, scenario runs S01-S12). Partly luck: the harness would not have caught it in a year
   either, because of (2) and (3).
2. **The soak measures correctness, not latency.** `soak/metrics.py` samples only memory;
   `soak/checker.py` checks queue depth, merge progress, and checkpoint correctness. No
   query-latency assertion exists anywhere; slow inserts just make the run take longer. The R4
   `trace_log` review looked at the **on-CPU** top — lock waits are off-CPU and invisible there.
3. **Wrong concurrency shape.** The soak runs `--workers 6`, all INSERT/OPTIMIZE. Reads are
   checkpoint-time scalars between phases. The production symptom — independent READERS stalling
   behind a writer holding the parts lock — is simply not modeled: nobody queues on the lock.
   Also parts are small and RustFS is loopback, so absolute hold times were milliseconds; on real
   S3 (50-200 ms RTT, multi-GB parts) they are seconds.

Existing but unused instrumentation: `ProfileEvents::PartsLockHoldMicroseconds` /
`PartsLockWaitMicroseconds` (`ProfileEvents.cpp:619-620`) — `DataPartsLock` already carries
`wait_watch` + `lock_watch` (`MergeTreeData.h:111-113`). A single `system.events` before/after
sample around the R4 soak would have exposed the anomaly.

## P1. Soak harness: latency + lock metrics {#p1-soak-harness}

- **P1a — lock metrics in the ticker.** The phase-3 metrics ticker samples per-tick deltas of
  `PartsLockHoldMicroseconds`, `PartsLockWaitMicroseconds` (plus `ContextLockWaitMicroseconds`)
  from `system.events` on both replicas. First runs observe-only to calibrate; then a gate: a
  per-tick hold delta above the calibrated budget fails the run with the tick timestamp (so it
  can be correlated with the workload phase and chaos windows).
- **P1b — latency canary.** A dedicated thread issues a cheap point `SELECT` (for example
  `count()` on a small fixed table, and one `system.parts` scan) every ~100 ms and records
  latencies. Assertion: canary p99 outside chaos windows below a threshold. This models the
  production symptom directly — a reader stalled behind a writer-held lock.
- **P1c — readers in the workload.** Phase-3 gets N continuous reader threads over the active
  tables (point reads + small range scans), so writer-held locks always have someone queuing on
  them. Reader errors/timeouts are workload failures.

## P2. Debug fail-fast guard: no network I/O under hot locks {#p2-debug-guard}

The strongest prevention: make the bug class impossible to land silently.

- A thread-local depth counter set by `DataPartsLock` / `DataPartsSharedLock`
  constructors/destructors ("this thread holds the parts lock").
- `chassert(!DataPartsLockIsHeldByThisThread())` at the entry of synchronous network primitives:
  the CA store operations (blob put, promote, repoint, drop, `ForceFresh` validation, journal
  writes) and optionally the generic S3/HTTP request path.
- Precedent pattern in the codebase: `MemoryTrackerBlockerInThread`-style thread-local scope
  blockers.
- Effect: any future "network under parts lock" trips instantly in every debug build run —
  gtest, stateless, integration — with a stack pointing at both the lock scope and the network
  call. Task 1.1 would have been caught on landing day.
- Expected immediate findings: the plain-engine `MergeTreeSink` path commits the disk transaction
  via `commit(lock)` — on a CA disk that is a publish under the parts lock (already flagged in
  the durability spec as a perf note). The guard forces us to enumerate and fix or explicitly
  whitelist every remaining instance — that is a feature, and it mechanizes Audit A's
  maintenance.

## P3. Audit A: "expected-instant" operations that are S3-slow on CA {#p3-audit-a}

Callers throughout MergeTree assume certain storage operations are near-instant because they are
near-instant on a plain disk: directory/file rename, `exists`, unlink, listing, small metadata
reads, `createDirectory`. On CA some of these synchronously issue S3 round trips.

- **Step 1 — classify.** For every public operation the CA layer implements
  (`ContentAddressedMetadataStorage`, `ContentAddressedTransaction`, object-storage entry
  points): latency class IN-MEMORY / LOCAL-DISK / S3-NETWORK; for network ops — which S3 calls
  and how many round trips; whether the class is state-dependent (e.g. `moveDirectory`:
  staged-source tmp→final = in-memory re-key vs committed-source = S3 repoint; `exists` with
  `Freshness::ForceFresh` = HEAD vs cached = memory).
- **Step 2 — lock-context sweep.** For every S3-NETWORK operation, enumerate call sites and
  classify the lock context each runs under: `data_parts` lock, table locks, background-task
  mutexes, our own CA mutexes (see Audit B), Keeper client sections. Deliverable: a table
  operation × call site × lock context × verdict (safe / perf hazard / correctness hazard), with
  backlog items for every hazard.
- Scope anchor: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (97 files;
  Backend/Core/Formats/Gc/Parts/Pool/Primitives/Tools).

## P4. Audit B: fork-introduced mutexes in the CA code {#p4-audit-b}

We introduced multiple mutexes/locks of our own (the whole `ContentAddressed/` tree is
fork-new). Do we understand their side effects?

For EVERY mutex/`SharedMutex`/condition variable in the CA tree, produce: what it protects; the
longest operation possible inside its critical section (network I/O? Keeper calls? unbounded
folds? cv waits — with or without timeout?); who the waiters are (query threads? background
loops? the lease renewer?); whether waiting has a cutoff/timeout or can wedge indefinitely; and
interaction with fencing (a beat-blocked lease renewal under a busy mutex was already a real
incident — the P3.1 S13-wedge: the GC fenced a lease whose renewer was blocked). Deliverable: an
inventory table + a hazard list with backlog items.

## P5. Periodic instrumented runs {#p5-instrumented-runs}

Not per-run — once per milestone, or when P1 metrics flag something unattributable:

- **Off-CPU flamegraph** of a soak with P1c reader load (tooling already exists in the repo —
  `offcpu.folded`/`offcpu.svg` were produced before): reader threads blocked in `lockParts` show
  up as wide futex stacks; the holder's stack shows what it was doing.
- **`perf lock contention -abv -p <pid>`** on kernels that support it (lock contention events
  with stacks, covers futex-backed userspace mutexes).
- **bpftrace uprobes** on `DB::SharedMutex` lock/unlock to measure per-lock hold-time
  distributions with stacks — this is the tool for finding the NEXT uninstrumented hot lock, not
  for the parts lock (which ProfileEvents already covers).

## P6. Production-shape observability {#p6-prod-observability}

Enable `metric_log` in the nightly image; document an alert rule on the rate of
`PartsLockHoldMicroseconds` (and `PartsLockWaitMicroseconds`) per table server-wide in the
monitoring guide, next to the existing Keeper alerts. A production stall of this class must page
before users report it.

## P7. Closing validation: SELECT anomaly review via query_log {#p7-query-log-review}

After the `renameParts` durability fix lands and P1 exists: run the standard soak, then mine
`system.query_log` `ProfileEvents` per query (`PartsLockWaitMicroseconds`, S3 request counters,
`Keeper` wait events) for SELECT outliers — queries whose lock-wait or network counters are
anomalous relative to the run's distribution. This both validates that the publish really left
the lock (the fix's perf claim) and becomes a reusable post-soak analysis step.

## Sequencing {#sequencing}

1. Land the `renameParts` durability fix (plan in review) — removes the known worst instance.
2. P2 guard (small C++ change + chassert sweep) — prevents recurrence; expect it to flag the
   `MergeTreeSink`-on-CA publish-under-lock immediately: triage that finding (fix or whitelist).
3. P1a/b/c in the soak harness (Python only) + calibrate thresholds on two runs.
4. Audits A and B (parallel read-only subagent sweeps + synthesis) → backlog items.
5. P7 query_log review wired into the post-soak routine; P5 one instrumented run to baseline;
   P6 monitoring-guide addition.
