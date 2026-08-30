# Wire-keys phase 2 — throughput before/after

Both sides built with the same compiler and flags (`clang-21`, `SANITIZE=OFF`, `ENABLE_BENCHMARKS=ON`)
and run back to back on one machine.

**Read the repetition medians as the result; read the 21-row single run as breadth.** The two sides of
the single run were NOT taken under comparable load: the 1-minute load average was 16.09 while the
BEFORE side ran and 3.04 while the AFTER side did. A loaded BEFORE side measures slower, so that run
understates the regression rather than inventing one — but it is not a fair pair. The repetition runs
were quiet and matched (0.64 and 0.92) and are the numbers to quote. `cpu_scaling_enabled` was true
throughout, which is another reason to trust relative medians over absolute nanoseconds.

- BEFORE: worktree at `65ec8688cdb` (the commit preceding phase-2 Task 1), `contrib` shared with the
  main checkout — verified identical, `git diff --submodule 65ec8688cdb..HEAD -- contrib` is empty.
- AFTER: `dbd7ed65635` (phase-2 complete bar the final gate).
- Harness: `benchmark_cas_ref_protocol`. Raw JSON in `bench-wire-keys-phase2/`
  (`before.json` / `after.json`, and the `--benchmark_repetitions=3` pair `before_rep.json` /
  `after_rep.json`).

The in-file baseline table at the top of `benchmark_cas_ref_protocol.cpp` predates this design and is
NOT the before side.

## Single run, all five benchmark families (breadth; see the load caveat above)

| benchmark | before ns | after ns | delta |
|---|---:|---:|---:|
| `BM_EncodeRefLogTxn` | 500.7 | 533.5 | +6.5% |
| `BM_ApplyRefLogTxn/100` | 696.6 | 731.8 | +5.0% |
| `BM_ApplyRefLogTxn/1000` | 720.7 | 740.0 | +2.7% |
| `BM_ApplyRefLogTxn/10000` | 778.0 | 769.1 | −1.1% |
| `BM_ApplyRefLogTxn/100000` | 754.2 | 774.2 | +2.7% |
| `BM_FlushInstall/100` | 16 365.8 | 17 004.5 | +3.9% |
| `BM_FlushInstall/1000` | 169 052.7 | 174 312.1 | +3.1% |
| `BM_FlushInstall/10000` | 1 726 937.8 | 1 777 901.6 | +3.0% |
| `BM_FlushInstall/100000` | 20 826 010.4 | 22 013 455.7 | +5.7% |
| `BM_FlushInstallUniqueOwner/100` | 1 665.7 | 1 671.7 | +0.4% |
| `BM_FlushInstallUniqueOwner/1000` | 1 951.5 | 1 923.3 | −1.4% |
| `BM_FlushInstallUniqueOwner/10000` | 3 417.2 | 3 573.6 | +4.6% |
| `BM_FlushInstallUniqueOwner/100000` | 12 592.6 | 11 364.7 | −9.8% |
| `BM_ReplayHistory/100` | 419 975.4 | 434 884.9 | +3.6% |
| `BM_ReplayHistory/1000` | 1 689 316.7 | 1 815 702.4 | +7.5% |
| `BM_ReplayHistory/10000` | 15 512 096.0 | 16 884 521.0 | +8.8% |
| `BM_ReplayHistory/100000` | 168 618 629.5 | 181 455 532.5 | +7.6% |
| `BM_SnapshotEncode/100` | 13 386.4 | 13 950.8 | +4.2% |
| `BM_SnapshotEncode/1000` | 136 327.0 | 140 891.0 | +3.3% |
| `BM_SnapshotEncode/10000` | 1 351 584.9 | 1 478 659.6 | +9.4% |
| `BM_SnapshotEncode/100000` | 14 057 280.3 | 14 742 480.0 | +4.9% |

## The result: medians of three repetitions, both sides quiet and load-matched

| benchmark | before ns | after ns | delta |
|---|---:|---:|---:|
| `BM_EncodeRefLogTxn` | 498.6 | 523.7 | +5.0% |
| `BM_SnapshotEncode/10000` | 1 334 071.6 | 1 403 072.5 | +5.2% |
| `BM_SnapshotEncode/100000` | 13 768 119.8 | 14 799 105.4 | +7.5% |
| `BM_ReplayHistory/10000` | 15 545 732.4 | 16 817 412.0 | +8.2% |
| `BM_ReplayHistory/100000` | 168 806 812.5 | 180 525 024.0 | +6.9% |

## Reading

Every measured path costs single-digit percent more, with no outlier and nothing multiplicative. The
medians and the single run agree on the shape and on every headline figure, but not to a fixed
tolerance: the gaps are 1.50, 0.67, 0.67, 4.23 and 2.61 points, and the two `BM_SnapshotEncode` sizes
swap which of them is worse between the two runs. Neither run resolves differences of a point or two
between sizes; both resolve the conclusion.

The largest consistent cost is on DECODE (`BM_ReplayHistory`, the fold/recovery profile: +6.9% to
+8.8%), which is where the design said the primary evidence would be — the dominant risk was always
the longer keys themselves, paid in bytes written, compressed, decompressed and compared. Encode
follows at +5% to +7.5%. `BM_ApplyRefLogTxn` and `BM_FlushInstallUniqueOwner` straddle zero, which is
what a path dominated by work other than key handling should do; the −9.8% outlier there is noise, not
a speedup.

## What this does NOT cover

The harness measures `cas_ref_log` and `RefSnapshot` only. `PartManifest`, `FoldSeal`, `RefCatalog`
and raw `cas_run` streaming have no throughput coverage — the design says to reach them by EXTENDING
this harness rather than adding a second one, and that work belongs to phase 3 along with the
stored-versus-decompressed byte accounting and the hot-path assembly review.
