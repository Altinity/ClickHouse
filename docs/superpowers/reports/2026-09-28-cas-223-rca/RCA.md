# CAS-223 RCA: why the TSan and MSan CAS stateless shards do not fit the 6-hour budget

Scope: CI run https://github.com/Altinity/ClickHouse/actions/runs/36347426173 (PR #2440, head `58f8458c707`).
All paths are relative to `tmp/cas223/`. Phase reports: `phase1_ci_evidence.md`, `phase2_hypotheses.md`, `phase3_repro.md`,
`phase4_fixes.md`. Nothing was committed or pushed; code changes are in `patches/` and in the worktree
`~/workspace/ClickHouse/cas223-perf` (uncommitted).

## Verdict in one paragraph

The loss is not thread count and not a generic sanitizer tax. It is a positive-feedback loop between the GC and the one pool-wide
catalog object, with a constant control-plane request overhead underneath it. Every CAS foreground operation re-reads and fully
decodes `cas/ref_catalog` (all namespaces of the pool), and the only thing that shrinks that object -- the GC's pre-fold drain,
which erases dropped tables' `removing` rows -- was the one catalog writer left outside the per-pool hot-key lane. Under a steady
stream of CREATE/DROP it loses nearly every optimistic race, the round cannot proceed past it, GC stops, the catalog grows, every
foreground op slows linearly with it, and the drain's own read-to-write window grows, so it loses even more. The per-entry decode is
allocation-heavy, which TSan and MSan instrument at ~20x the release cost, so the loop that is a mild drift in release and ASan
becomes the 4x (TSan) and 7x (MSan) ramp seen in CI.

## Root causes, with evidence

### RC1 (proven): the GC drain is starved by the server's own catalog writers

- Code: `CasRefCatalog::deleteCompletedRemovingAtSnapshot` erases with its own read + `op.replace` (`Pool/CasRefCatalog.cpp:452-562`
  at the PR head), one row per full rewrite; every other catalog mutation goes through `casUpdateImpl` -> `CasHotKeys::submit`,
  the per-key FIFO that exists precisely to stop in-server writers of one key from racing (`Backend/CasHotKeys.h:21-38`). The drain
  gates the round (`Gc/CasGc.cpp:473-481`).
- Local, TSan CI binary, GC on: one GC round completed in 286 s; the drain phase ran 232.7 s and finished 7 s after the load stopped;
  stack samples show the GC thread sleeping in the drain's post-conflict backoff (`phase3_repro.md`, experiment 4).
- Local, release, RustFS with CI settings: 1 round OK then 5 rounds `Aborted` at 76-81 s with `CAS ref catalog erase ... could not be
  committed`; catalog 0 -> 7980 rows, none reclaimed (`runs/s3_relbase/gc_rounds.tsv`, `samples.tsv`).
- CI: the release CAS lane's default pool logs the same message 10 times, completes 12 rounds in 67 min, rounds of 9-27 min
  (`ci_release_default_pool_gc_rounds.txt`); ASan rounds grow 22 s -> 10 min (`ci_asan1_default_pool_gc_rounds.txt`).
- Unit test that fails on the PR head and passes with the fix: `CASCatalogLifecycleReconciler.DrainDoesNotLoseRacesToThisServersCatalogWriters`
  (PR head: "the drain lost 3 catalog races to its own server's writers", `cas223-perf/build/test_failing_first.log`).

### RC2 (proven): every foreground operation costs O(catalog entries) CPU, amplified ~20x by TSan

- Code: `CasRefCatalog::read` = GET + `decodeRefCatalog` of every entry + `CatalogLifeIndex` + snapshot copy; called before every
  ref-log append (`CasRefLedger::commitRefChunk`, `Pool/CasRefLedger.cpp:3394`, `:3425`), in create/drop/runtime paths, ~29 times per
  table lifecycle (`CASOtherGet`).
- Controlled experiment (GC off, catalog grown with idle tables, same request count per run): release +12-16 ms per 1000 entries per
  create/insert/drop cycle, TSan +260-330 ms per 1000 entries; idle tables on the local disk as the control (`phase3_repro.md`, exp. 2).
- `perf` under TSan at 3000 entries: `CasRefCatalog::read` is 56.2 % of all server CPU; `commitRefChunk` is 67 % of its callers
  (`perf/tsan_w_children.txt`).

### RC3 (measured, not fixed): constant control-plane request amplification

One CREATE / 3 x one-part INSERT / SELECT / DROP cycle on the same RustFS (`runs/req_cas.tsv`, `runs/req_plain.tsv`):

| | CAS | plain S3 |
|---|---|---|
| GET | 83 | 12 |
| PUT | 64 | 31 |
| LIST / HEAD / DeleteObjects | 7 / 5 / 1 | 0 / 0 / 2 |

DROP of a 3-part table alone uploads 45 objects: 19 ref-log transactions, each a `_log` chunk plus a `_ckpt` rewrite, plus 6 manifests
and 1 catalog write (`runs/drop_trace/drop_ops.tsv`), serially, each append preceded by a catalog GET. This is the open
"part-removal repoint" item. It is the ~2.3x constant between CAS and plain S3 at the start of a run (TSan: 440 vs 195 ms per cycle).

### RC4 (measured, not fixed): GC round time grows with dead-life debris

With RC1 fixed, `defer_decision` (the global LIST of `cas/ns/stream/`) still grows 4.4 -> 45 s over 7 rounds, because dropped tables'
ref objects stay listed until the one-page-per-round janitor reaches them (`runs/s3_relfixA/gc_log_all.tsv`). Longer rounds let the
catalog oscillate higher between drains. Two attempted fixes were measured and rejected (`phase4_fixes.md`).

## Refuted or demoted

Thread count (ramp reproduces at a constant ~750 threads; ASan CAS with the same pools is fast), request count as the ramp (constant
per cycle), port exhaustion (no signature, reproduces without network), GC CPU competition (tests without tables flat in CI), the
MemoryWorker cascade (not observed), RustFS fsync (CI already runs `RUSTFS_DURABILITY_MODE=relaxed`; the ramp reproduces on the local
object store). The CI "CAS vs S3" comparison is also confounded: the TSan S3 lane runs with random query settings off
(`tests/clickhouse-test:5728`) and behind a 5 GiB filesystem cache on MinIO.

## Fixes and measured effect

- `patches/0001-cas-gc-drain-batch-erase-through-hot-key-lane.patch` (fix A + tests): the drain erases every proven row in one
  `casUpdateImpl` hold through the hot-key lane; same per-row proof, mandatory resolution read kept. Full `CAS*` gtest gate:
  2538 passed, 0 failed (`cas223-perf/build/test_cas_gate3.log`).
- `patches/0002-cas-catalog-reuse-decoded-snapshot-for-identical-bytes.patch` (fix B): `CasRefCatalog::readShared` keeps the GET but
  reuses the decoded snapshot for byte-identical bytes; used by the `commitRefChunk` admission re-reads.

Measured on RustFS with CI settings, 8 workers, GC on (`phase4_fixes.md` has the full tables):

| binary | workload | wall | GC rounds ok / aborted | catalog at end | ms per cycle, first -> last bucket |
|---|---|---|---|---|---|
| TSan, PR head (CI binary) | 8 x 250 | 219.3 s | 1 / 1 | 1985 | 477 -> 1223 |
| TSan, A+B | 8 x 250 | 163.4 s | 3 / 0 | 980 | 440 -> 737 |
| TSan, plain S3 | 8 x 250 | 59.0 s | -- | -- | 195 -> 274 |
| release, PR head | 8 x 1000 | 569.4 s | 1 / 5 | 7842 | 300 -> 818 |
| release, A+B | 8 x 1000 | 449.7 s | 6 / 0 | 1107 | 337 -> 484 |
| release, plain S3 | 8 x 1000 | 143.9 s | -- | -- | 123 -> 143 |

Each row ran on a freshly emptied RustFS (`rig/rustfs_fresh.sh`); outputs in `runs/s3_tsan_*.out`, `runs/s3_relfixAB_final.out`,
`runs/s3_relbase_and_plain_fresh.out`.

## What is proven vs hypothesis

- Proven by measurement and code: RC1, RC2, RC3, RC4 as mechanisms; A removes the drain starvation (every drain 12-116 ms, all
  rounds succeed); A+B cut the TSan run by 25 % and its last-bucket cycle time by 40 %; in release by 21 % and 41 %.
- Hypothesis (not measurable from CI artifacts): that the CI TSan/MSan ramp is dominated by RC1+RC2 rather than RC3+RC4. Supported by
  the per-test ramp shape (MergeTree tests only, ordered MSan > TSan > ASan, flat for tests without tables) and by the same GC failure
  message in the CI release log; the sanitizer CAS jobs exported no server log or system tables, so the catalog size in CI is inferred,
  not observed. A CI run of the patched build with `system.cas_gc_log` and catalog size dumped before the stop would close this.
- Not claimed: that A+B alone bring the shards under 6 h. The fix removes the runaway; RC3 (~2.3x per cycle vs plain S3 under TSan) and
  RC4 remain, and plain S3 is still 2.8x (TSan) and 3.1x (release) faster on the local repro.

## What remains

1. RC3: collapse the per-part repoint transactions of DROP / part removal into one namespace-level transaction, and stop rewriting
   `_ckpt` on every append (19 `_ckpt` PUTs for one 3-part DROP).
2. RC4: move dead-life object deletion off the round's critical path (background, or bulk delete where exactness allows), so the
   global enumeration stops re-listing debris.
3. RC2 remainder: the other full-catalog readers (`namespaceStillLogicallyPresent`, `dropNamespaceImpl`, `resolveNamespaceLife`,
   `acquireReadableRefTableRuntime`) still decode the whole object; a per-namespace catalog lookup would make foreground cost
   independent of pool size.
4. CI observability: dump `system.cas_gc_log`, `system.cas_log`, catalog entry count and `metric_log` before the stop in the CAS lanes.
5. Review: patch 0001 changes a GC-safety component (drain); it needs a CAS review before it goes anywhere.
