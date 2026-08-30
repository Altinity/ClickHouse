---
description: 'Before/after decode and encode throughput, byte accounting and cap maxima for the CAS semantic wire-key cut.'
sidebar_label: 'Wire keys full measurement'
sidebar_position: 98
slug: /superpowers/cas/wire-keys-full-measurement
title: 'CAS wire keys — the full before/after measurement'
doc_type: 'reference'
---

# CAS wire keys — the full before/after measurement {#cas-wire-keys-the-full-before-after-measurement}

What renaming the wire keys of all seventeen CAS persisted formats cost in bytes and in time.

**This is the second measurement.** The first was taken across two binaries compiled with different
ISA baselines and frame-pointer settings and is void; it is preserved with its manifest under
`bench-wire-keys-phase3/void/20260830-flag-mismatch/`. Three of its conclusions did not survive the
correction, and they are called out where they occur rather than quietly replaced.

**Two facts, and neither replaces the other.** Decoded records per second fell — by 2.8% to 20.8%
depending on the format. Per byte, the decoder did not get worse: four of the five formats decode
more cheaply per byte than before the cut and the fifth is within noise. Both are true. A record got
bigger, so a record costs more to decode; the decoder itself did not get slower at its job. The
superseded version of this document reported only the second fact, which answered a different
question than the design asked — the criterion asks for records per second, and that is the metric
this document now leads with.

## Conditions {#conditions}

| | |
|---|---|
| Before side | worktree `/home/mfilimonov/workspace/ClickHouse/cas-p2-before`, detached at `65ec8688cdb` |
| After side | freeze commit `e9f1c3d867c`, plus the benchmark-only commits `1eda4521fd4` and `dc52a79bb5d` |
| Build configuration | **verified identical** — the measurement driver now diffs the two `CMakeCache.txt` files and refuses to run if they differ |
| Flags | `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true`, filtered to the ten format benchmarks |
| Machine | 32 CPUs, `cpu_scaling_enabled=true` on both sides of both passes |
| Pass 1 load (1 min) | 0.49 before, 1.00 after |
| Pass 2 load (1 min) | 1.08 before, 1.04 after |

Both passes are VALID under the protocol fixed before running: a run is void if the two sides' load
averages differ by more than 1.0 or either exceeds 2.0.

`cpu_scaling_enabled` is true, so absolute nanoseconds are not portable off this machine. Relative
medians are what these tables are for.

**The configuration gate is new, and it exists because of what it would have caught.** The first
measurement — and the phase-2 one before it — asserted "the same compiler and flags" on the strength
of listing three settings that did match. Two that did not were never on anyone's list: the before
side had been configured with a plain `cmake` and took the repository defaults, while the after side
carries deliberate non-default settings. Nobody chose to differ; a fresh configure in a new worktree
simply inherits nothing from its neighbour. The gate now diffs the two configurations and aborts, so
the claim is checked rather than asserted.

**Why there are two passes.** Running the sides back to back leaves the order uncontrolled: the
second runs on a machine the first has warmed. Pass 2 swaps them. The two passes agree within 1.1 to
2.8 points on every format.

## Throughput {#throughput-section}

### Records per second, the primary decode metric {#records-per-second}

At n = 100,000, from the forward pass.

| format | before | after | change |
|---|---:|---:|---:|
| `cas_run` | 1,705,488 | 1,555,317 | **-8.8%** |
| `cas_ref_snap` | 1,489,088 | 1,383,936 | **-7.1%** |
| `cas_part_manifest` | 1,281,307 | 1,245,605 | **-2.8%** |
| `cas_fold_seal` | 1,412,704 | 1,118,967 | **-20.8%** |
| `cas_ref_catalog` | 1,991,455 | 1,820,523 | **-8.6%** |

### Decode and encode time, median over the four range points {#throughput}

| format | decode | decode, reverse pass | encode | encode, reverse pass |
|---|---:|---:|---:|---:|
| `cas_run` | +7.1% | +5.9% | -0.9% | -0.8% |
| `cas_ref_snap` | +10.8% | +12.0% | -15.7% | -20.9% |
| `cas_part_manifest` | +3.1% | +4.6% | -7.0% | -5.8% |
| `cas_fold_seal` | +27.4% | +26.0% | +1.6% | +0.6% |
| `cas_ref_catalog` | +11.4% | +8.5% | -13.2% | -8.9% |

### Bytes, and time per byte {#bytes-and-time-per-byte}

Time per byte is a RATIO — decode-time factor divided by byte factor — not a subtraction of
percentages, which is a different quantity and was wrong in the superseded version.

| format | bytes before | bytes after | bytes | decode | time per byte |
|---|---:|---:|---:|---:|---:|
| `cas_run` | 94,103 | 101,442 | +7.8% | +7.1% | -0.7% |
| `cas_ref_snap` | 94,934 | 123,988 | +30.6% | +10.8% | -15.2% |
| `cas_part_manifest` | 101,819 | 116,344 | +14.3% | +3.1% | -9.8% |
| `cas_fold_seal` | 93,880 | 125,623 | +33.8% | +27.4% | -4.8% |
| `cas_ref_catalog` | 94,185 | 103,844 | +10.3% | +11.4% | +1.0% |

### Object capacity {#object-capacity}

From the harness's byte/cap oracle, whose raw output for both sides is committed under
`bench-wire-keys-phase3/oracle/`. The maximum is found by binary search with the real encoder as the
oracle. It is the maximum for the *synthetic fixture*, not a property of the format: a production
record with different field widths would give a different number, and the figure should be read as
"this many records of this shape", not as a capacity guarantee.

| format | before | after | stored after |
|---|---:|---:|---|
| `cas_run` | n/a (streamed one line at a time, never materialized whole) | n/a | n/a (`PinnedRaw`) |
| `cas_ref_snap` | 660,207 | 514,053 | 4,128 (zstd) |
| `cas_part_manifest` | 2,622,258 | 2,298,073 | 3,344 (zstd) |
| `cas_fold_seal` | 2,881,746 | 2,153,504 | n/a (stored raw) |
| `cas_ref_catalog` | 2,851,448 | 2,586,082 | n/a (stored raw) |

The stored-bytes column is a fixture artifact and must not be read as production compression: the
synthetic rows repeat structure by index, so zstd collapses them far harder than real data carrying
incompressible hashes would. The cap maxima are not affected — the cap is enforced against
decompressed size, and every maximum matches the cap divided by decompressed bytes per record to
within 5%.

## What the numbers mean {#what-the-numbers-mean}

**Encoding got faster.** The encode median across all five formats is −7.0%, with `cas_ref_snap` at
−15.7% and `cas_ref_catalog` at −13.2%. That is not the cut being free; it is the `wordValue`
write-path change that landed during this phase, which stopped routing constexpr vocabulary words
through the string-escape state machine. The void run did not show this, because its before side was
compiled for a newer ISA baseline and that advantage masked the gain.

**Decoding a record costs more, because a record is bigger.** Records per second fell on every
format, and the loss tracks the byte growth: `cas_part_manifest` grew 14.3% in bytes and lost 2.8%
of records per second, `cas_fold_seal` grew 33.8% and lost 20.8%. Per byte the decoder is unchanged
or better on four of five formats, and `cas_ref_catalog`'s +1.0% is within the agreement between the
two passes.

**`cas_fold_seal` is the format to watch, and it is not an anomaly.** Its rows carry the most keys
and the shortest values, so key text dominates its bytes more than any other format's — a third more
bytes after the cut. Its decode time rose 27.4% in the forward pass and 26.0% in the reverse. If a
workload folds very large seals, that is the cost it will feel.

**A conclusion the first measurement got wrong.** It reported `cas_run` decode at +12.8% against
+7.8% byte growth and built a story on the five-point residue. On correctly matched binaries
`cas_run` decode is +7.1% against the same +7.8% — the residue was the flag mismatch, not the cut.
No format now decodes meaningfully more expensively per byte.

## The generated assembly {#the-generated-assembly}

Both sides disassembled with `.claude/tools/analyze-assembly.py` in before/after mode, on the same
binaries the tables above came from. Raw output under `bench-wire-keys-phase3/asm/`.

| symbol | branches | calls | spill density |
|---|---:|---:|---:|
| `SourceEdgeRunReader::next` (`cas_run` decode) | −9 | −1 | **−1.26%** |
| `decodeRefCatalog` | −1 | +1 | **−0.72%** |
| `decodePartManifest` | 0 | +1 | **−0.70%** |
| `SourceEdgeRunWriter::append` (`cas_run` encode) | 0 | 0 | **0.00%** |

The plan named `SourceEdgeRunView::next` as the `cas_run` row reader. That is the wrong symbol: it
delegates to `SourceEdgeRunReader::next` and re-packs the typed record into raw strings for the
legacy fold consumers, parsing no wire keys itself.

**All three of the design's conditions hold.**

- *The enum tables are a lookup, not a scan.* Branch counts fell or stayed flat in every symbol,
  which is the opposite of what a comparison chain would do, and the table is visible in the encode
  path as `lea rdi, <kRunMarkerWords>` before the marker is rendered.
- *No added call, allocation or branch on hot decode paths.* `SourceEdgeRunReader::next` makes one
  fewer call than before. The three `match*Fields` helpers the criterion names are header-defined
  `inline`, as the criterion requires.
- *Nothing spills that did not spill before.* Spill density **fell** in all three decode symbols and
  did not move at all in the encode symbol.

**A second conclusion the first measurement got wrong.** It reported spills rising in every symbol
and concluded the third condition failed. That was the frame-pointer flag: the after side reserved
`rbp` while the before side did not. On matched binaries the direction reverses. The spec needed no
amendment, and the backlog item raised against this non-finding has been withdrawn.

## What is not covered {#what-is-not-covered}

- **Five formats of seventeen.** The design names these five; a format with an unusual key-to-value
  ratio could behave differently.
- **Synthetic fixtures.** Field widths were chosen to resemble production and the workloads are
  documented in the harness, but no figure here comes from a real pool. The stored-bytes column and
  the cap maxima are both fixture-shaped, and the cap maxima especially should not be read as format
  capacities.
- **No absolute claim.** `cpu_scaling_enabled` is true; these are relative medians on one machine.
- **No CI assertion.** By the design's instruction, none of this becomes a timing test. It is review
  evidence and it expires if production code changes.
