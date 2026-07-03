# Unattended night worklog — 2026-07-03

Plan (user, verbatim order): stop soak + summary → finish shard-mutation-queue → implement
gc-rebuild plan → config changes (root_shards=8 default, content_addressed_log default-on,
trim body under 128 KB) → 1h soak + triage → scenarios + repair → docker image.

## 1. Soak summary (copy-forward + scanner run, seed 20260703, phase 3, stopped at ~T+3h50m)

Run purpose: (a) validate copy-forward + self-remount under kill chaos, (b) test whether the
RustFS scanner cleans the overwrite leak.

**Findings:**

1. **Scanner experiment: NEGATIVE, root cause identified = rustfs#3231** (already documented +
   reverted in `configs/rustfs.env`, commit `188e0d3c92e`): overwrite of a >128 KiB object in an
   un-versioned bucket leaks the previous data dir. cas/refs = 39 GB of 45 GB pool at T+1h;
   167 GB at T+2h; per-key data dirs 1600 → 4088. `list_merged`/`walk_dir` LIST timeouts
   reproduced at ~600k files. Scanner reclaimed ZERO in >2h (bring-up works though — the old
   blocker was the unpinned mc).
2. **S3 stats** (per replica, T+1h): CasBlobHead 8.7M (gate revalidation — 57 HEAD per blob PUT);
   CasOtherCas 380k landed shard casPuts + 257k conflict attempts (40% avg, 92% peak);
   CasOtherGet 1.29M. Shard bodies 250-340 KB = journal tail (3157 events, 95% of body; live
   refs only 53 = 5 KB). Trim works but is fold-cadence-bound; trims stop entirely under LIST
   storms. → motivates shard-mutation-queue + trim-size config change (tonight's tasks).
3. **`mount_beat` introspection works**: 26-27 view-advance events per replica.
4. **BUG FOUND (mine, in copy-forward, S13-fix): FALSE `CORRUPTED_DATA` on real copy-forward.**
   `Build::copyForwardFromCondemned` verifies the payload by ONE-SHOT `CityHash128`, but the pool
   convention is the STREAMING `HashingWriteBuffer` hash (chunked CityHash128, block = 2048 B —
   `DBMS_DEFAULT_HASHING_BLOCK_SIZE`). Any payload > 2048 B diverges ⇒ verify falsely refuses ⇒
   attach brick returned: ch1 `ca_stress` readonly at 22:48:27 with
   `copyForwardFromCondemned: object .../a496d0c0... payload does not verify (payload hash
   9cf6e90d..., header hash a496d0c0..., expected a496d0c0...)`. Unit repro passed because test
   payloads were < 2048 B (single block = plain hash). Positive side: the copy-forward TRIGGER
   scenario reproduced in the wild (first real condemned-evidence reuse), and the fail-closed
   path behaved exactly as designed — the verdict was just wrong.
   **FIX (tonight, simple):** recompute via `HashingReadBuffer` over the payload + the
   `getHexUIntLowercase` → `hexToU128` chain the write path uses; RED test with a multi-block
   payload whose BlobId is minted by the streaming convention.
5. Oracle checkpoints passed until the brick (count 1.79M rows matched); `dangling=0` in the one
   fsck that completed; later fscks timed out (>180 s — the leak-bloated LIST, known B146/B154).
6. Chaos: 20+ faults fired; kills/restarts survived until finding 4 bricked ch1's attach.

Archive: `utils/ca-soak/logs/archive_copyforward_scanner_run_20260703T0130/` (ch logs 195 MB +
driver log). Pool destroyed (`down -v`).

## 2. Copy-forward hash fix — DONE (`9f92b3f5e92`)

`poolContentHash` (HashingReadBuffer through the write path's hex chain) replaces the one-shot
CityHash128 in `copyForwardFromCondemned`. Multi-block regression test red->green; the existing
verification tests re-minted with the shared `streamingHexOf` helper (plain `idOf` is a test-local
convention the verifier rightly refuses). Cas* 434/434 (+4 expected-red CasShardQueue).

## 3. Shard-mutation-queue (spec 2026-07-03-cas-shard-mutation-queue)

DONE: queue `20e21d96f8a`, metrics+docs `11ef5ec5146`. Cas* 438/438; CasShardQueue 5/5 x8.
Two real races found by the stress test during implementation:
- slow-exiting waiter erased a SUCCESSOR queue by key => two leaders => conflicts; erase is now
  identity-checked (map entry must still be OUR queue);
- test-harness: counters had to be filtered to cas/refs keys (bootstrap adopts + mount renewals
  also casPut), and co-batching made deterministic via a queue-depth probe.

## 4. GC rebuild (spec+plan 2026-07-03-cas-gc-rebuild)

- Task 1 TLA+ gate DONE (`bd16aff956d`): GRebuild + witness + 3 sabotages; SabotageRebuildDropEdge
  needed a `lostRefs` ghost set (dropping the folded ref also dropped the invariant's witness).
  Stage-1 clean; all old cfgs re-verified.
- Task 2 guard DONE (`6bd21f28c78`): per-shard trim-evidence refusal + (б) absent-seal audit.
  First cut keyed the audit on coverage EMPTINESS and tripped 9 tests — a present seal with empty
  per_ns_shard is a legitimate empty-universe generation; re-keyed on OBJECT absence.
- Task 3 rebuildBaseline DONE (`24b3362bfb8`): engine = existing bricks; health check gates FORCE
  (gen-0 bootstrap over trim-proving journals is NOT healthy); REAL FIND during testing —
  **pipeline blindness**: a blob with zero rebuilt edges has no run row and never transitions to
  zero => never reclaimed; the rebuild now condemns zero-edge physical blobs itself (head-captured
  tokens, minted round, floor-gated graduation). CasGcRebuild 7/7, Cas* 448/448.
- Tasks 4-5 (SYSTEM + clickhouse-disks wiring + docs) dispatched to a sonnet subagent.

## 5. Config changes — DONE (`cbe0ffb7608`)

content_addressed_log default-ON in programs/server/config.xml (experimental => keep enabled
recommendation); soak root_shards 64 -> 8 (16 files); gc_trim_body_soft_limit 8 MiB -> 96 KiB
(bodies stay under the RustFS 128 KiB inline threshold => rustfs#3231 leak sidestepped).

## 6. 1h soak (pending: after subagent lands Tasks 4-5 + full rebuild of the binary)


