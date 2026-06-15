# CA: `fsck` per-LIST timeout + progress (design)

**Date:** 2026-06-15. **Branch:** `cas-mergetree-poc`. **Status:** brainstormed (self, unattended) → spec.

## Problem
`clickhouse-disks fsck` appears to hang for tens of minutes on a large, churning pool: `runFsck`'s `listAll` issues a paginated `ListObjectsV2`; when RustFS returns a `500`/timeout on a page under load, the AWS SDK retry-loops it (up to 501×) and `runFsck` produces **no output until the very end** (`CommandFsck.cpp:46-51`). Two tool weaknesses: **no progress output** (can't tell working-vs-hung) and **no bound** (a single stuck page wedges the command). Evidence: findings doc §A; B158.

## Design

### 1. Progress callback in `runFsck` (`CasFsck.{h,cpp}`)
`runFsck(Store &, bool detail, FsckProgress on_progress = {})`, where
```cpp
using FsckProgress = std::function<void(std::string_view phase, uint64_t objects, uint64_t pages)>;
```
`listAll` invokes `on_progress("listing <prefix>", objects_so_far, pages)` every `PROGRESS_PAGES` (e.g. 16) pages; the tree-walk / HEAD-reconciliation phases call it periodically too. Default `{}` = no-op (existing callers unaffected).

### 2. Overall deadline in `CommandFsck` (the user-visible bound)
A single stuck `ListObjectsV2` cannot be interrupted from inside `listAll` (the SDK call blocks synchronously). So bound it at the **command** level: run `runFsck` on a worker via `std::async`, and `future.wait_for(timeout)` in the command thread. On timeout, print a clear diagnosis and exit non-zero; the detached SDK call dies with the process (acceptable for a one-shot CLI). New option `--timeout <seconds>` (default **600**; `0` = unbounded).
- On timeout: `std::cerr << "fsck: exceeded <N>s — likely a RustFS LIST stall under load. Run against a QUIESCED pool (stop the workload), or see B158."` then `throw` (non-zero exit).
- Progress lines go to `std::cerr` (so the `reachable=…` summary on `std::cout` stays machine-parseable).

### 3. Wiring
`CommandFsck::executeImpl`: parse `--timeout`; build a progress callback that writes to `std::cerr` (rate-limited by the `PROGRESS_PAGES` cadence); launch `runFsck` via `std::async(std::launch::async, …)`; `wait_for`. Keep the existing `--detail` and the `dangling>0` non-zero exit.

## Error handling
- Timeout → clear stderr message + non-zero exit (not a silent hang). Not a fallback; the scan genuinely didn't complete.
- `runFsck` still throws `FILE_DOESNT_EXIST`/etc. on real faults — propagated.
- `--timeout 0` preserves today's unbounded behavior for callers who want it.

## Testing
- `runFsck` with a progress callback over the in-memory/local test store: callback is invoked at least once and the final counts are unchanged (progress is observational, never alters the report).
- A `runFsck` over a populated test store still returns the correct `reachable/dangling/unreachable` (no regression).
- (Timeout path is process-level/CLI — covered by the soak's quiesced-checkpoint usage + manual; a unit test for `std::async` wait_for timing is brittle and omitted by design.)

## Scope
In: `runFsck` progress callback; `CommandFsck` `--timeout` + worker-thread bound + stderr progress. Out: changing the S3 client retry budget (a disk-config lever, noted in B158); the GC/op-count fixes that remove the stall's *cause* (separate items).
