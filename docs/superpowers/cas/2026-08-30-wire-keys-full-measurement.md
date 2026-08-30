---
description: 'Before/after encode and decode throughput, byte accounting and cap maxima for the CAS semantic wire-key cut.'
sidebar_label: 'Wire keys full measurement'
sidebar_position: 98
slug: /superpowers/cas/wire-keys-full-measurement
title: 'CAS wire keys — the full before/after measurement'
doc_type: 'reference'
---

# CAS wire keys — the full before/after measurement {#cas-wire-keys-the-full-before-after-measurement}

This is the quantitative half of the campaign's evidence: what renaming the wire keys of all
seventeen CAS persisted formats cost in bytes and in time, measured rather than argued.

**The headline, and it needs its denominator to read correctly.** Decode times grew by a median of
12 to 14 percent depending on the pass, and `cas_fold_seal` decode grew 27%. Read alone those
numbers look like a problem. Read against the byte growth of the same fixtures they are the opposite: `cas_fold_seal`'s encoded bytes
grew 34%, so its decoder got *cheaper per byte*, and the same holds for `cas_ref_snap` and
`cas_part_manifest`. The cost is the extra bytes, which is exactly what the design says the dominant
risk is. Only two formats decode more slowly than their bytes grew, and by single-digit margins.

## Conditions {#conditions}

| | |
|---|---|
| Before side | worktree `/home/mfilimonov/workspace/ClickHouse/cas-p2-before`, detached at `65ec8688cdb` |
| After side | freeze commit `e9f1c3d867c`, plus the benchmark-only commits `1eda4521fd4` and `dc52a79bb5d` |
| Before binary | `sha256 cde34568041f462804ac04dfd08f9565626088d556b4513a425e0351b6b58e34` |
| After binary | `sha256 070b777eadbaeeac93447128fbd738398a6cb311e49fd4fb8de73bf8bc66db49` |
| Flags | `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true`, filtered to the ten format benchmarks |
| Machine | 32 CPUs, 5756 MHz, `cpu_scaling_enabled=true` on both sides of both passes |
| Pass 1 load (1 min) | 1.13 before, 1.24 after |
| Pass 2 load (1 min) | 0.45 before, 0.88 after |

Both passes are VALID under the protocol fixed before running: a run is void if the two sides' load
averages differ by more than 1.0 or either exceeds 2.0. Neither did, and no run had to be voided, so
`bench-wire-keys-phase3/void/` does not exist.

`cpu_scaling_enabled` is true, so absolute nanoseconds are not portable off this machine. Relative
medians are what these tables are for.

**Why there are two passes.** The protocol asks for the two sides back to back, which leaves the
order itself uncontrolled: the second side runs on a machine already warmed by the first. Pass 2
runs the same measurement with the sides swapped, so that advantage lands on the opposite side. The
two passes agree where it matters — `cas_fold_seal` decode is +26.8% in pass 1 and +27.6% in pass 2,
0.8 points apart with the order reversed — so ordering does not explain the result. Where they
disagree is instructive in its own right: the largest gaps are all small, fast *encode* cases whose
deltas sit near zero, which is the noise floor making itself visible rather than a measurement
changing its mind.

## Throughput {#throughput}

Every figure is the median of three repetitions. The final column repeats the comparison from the
reverse-order pass, so each row carries its own reproducibility check rather than relying on a
summary statistic.

### decode — median of 3 repetitions, nanoseconds per call {#decode-throughput}

| format | n | before | after | delta | delta, reverse pass |
|---|---:|---:|---:|---:|---:|
| `cas_run` | 100 | 58,101 | 66,157 | +13.9% | +7.1% |
| `cas_run` | 1,000 | 569,207 | 657,627 | +15.5% | +11.1% |
| `cas_run` | 10,000 | 5,743,505 | 6,411,518 | +11.6% | +8.7% |
| `cas_run` | 100,000 | 57,277,221 | 63,977,969 | +11.7% | +7.6% |
| `cas_ref_snap` | 100 | 61,267 | 72,659 | +18.6% | +21.1% |
| `cas_ref_snap` | 1,000 | 606,895 | 709,150 | +16.8% | +16.0% |
| `cas_ref_snap` | 10,000 | 6,426,105 | 7,093,107 | +10.4% | +11.0% |
| `cas_ref_snap` | 100,000 | 65,667,170 | 73,398,072 | +11.8% | +9.7% |
| `cas_part_manifest` | 100 | 75,288 | 81,220 | +7.9% | +6.4% |
| `cas_part_manifest` | 1,000 | 730,492 | 794,934 | +8.8% | +8.6% |
| `cas_part_manifest` | 10,000 | 7,390,689 | 7,919,272 | +7.2% | +9.0% |
| `cas_part_manifest` | 100,000 | 74,540,536 | 81,441,539 | +9.3% | +8.2% |
| `cas_fold_seal` | 100 | 68,696 | 86,392 | +25.8% | +25.7% |
| `cas_fold_seal` | 1,000 | 648,431 | 837,958 | +29.2% | +27.7% |
| `cas_fold_seal` | 10,000 | 6,537,003 | 8,355,728 | +27.8% | +31.9% |
| `cas_fold_seal` | 100,000 | 68,790,919 | 86,480,521 | +25.7% | +27.4% |
| `cas_ref_catalog` | 100 | 49,494 | 56,852 | +14.9% | +12.2% |
| `cas_ref_catalog` | 1,000 | 485,585 | 542,666 | +11.8% | +12.7% |
| `cas_ref_catalog` | 10,000 | 4,907,725 | 5,565,026 | +13.4% | +11.6% |
| `cas_ref_catalog` | 100,000 | 48,853,086 | 56,822,699 | +16.3% | +15.8% |

### encode — median of 3 repetitions, nanoseconds per call {#encode-throughput}

| format | n | before | after | delta | delta, reverse pass |
|---|---:|---:|---:|---:|---:|
| `cas_run` | 100 | 13,791 | 14,479 | +5.0% | +3.6% |
| `cas_run` | 1,000 | 133,148 | 140,187 | +5.3% | +4.3% |
| `cas_run` | 10,000 | 1,323,527 | 1,446,852 | +9.3% | +0.3% |
| `cas_run` | 100,000 | 13,843,239 | 14,846,779 | +7.2% | -0.7% |
| `cas_ref_snap` | 100 | 9,186 | 9,309 | +1.3% | -3.0% |
| `cas_ref_snap` | 1,000 | 93,842 | 96,025 | +2.3% | -1.5% |
| `cas_ref_snap` | 10,000 | 924,971 | 930,695 | +0.6% | +0.3% |
| `cas_ref_snap` | 100,000 | 9,005,780 | 9,992,300 | +11.0% | +7.5% |
| `cas_part_manifest` | 100 | 12,921 | 12,743 | -1.4% | +1.6% |
| `cas_part_manifest` | 1,000 | 118,719 | 123,710 | +4.2% | +4.0% |
| `cas_part_manifest` | 10,000 | 1,185,506 | 1,252,363 | +5.6% | +7.4% |
| `cas_part_manifest` | 100,000 | 12,381,122 | 12,810,022 | +3.5% | +4.6% |
| `cas_fold_seal` | 100 | 12,504 | 12,569 | +0.5% | +1.1% |
| `cas_fold_seal` | 1,000 | 114,490 | 117,623 | +2.7% | +2.4% |
| `cas_fold_seal` | 10,000 | 1,144,133 | 1,180,101 | +3.1% | +4.3% |
| `cas_fold_seal` | 100,000 | 12,083,243 | 12,190,642 | +0.9% | +2.1% |
| `cas_ref_catalog` | 100 | 6,757 | 7,128 | +5.5% | +9.9% |
| `cas_ref_catalog` | 1,000 | 65,163 | 68,513 | +5.1% | +10.1% |
| `cas_ref_catalog` | 10,000 | 626,523 | 685,146 | +9.4% | +9.0% |
| `cas_ref_catalog` | 100,000 | 6,690,341 | 7,079,379 | +5.8% | +4.7% |

### records per second, at n = 100,000 {#records-per-second}

| format | direction | before | after | change |
|---|---|---:|---:|---:|
| `cas_run` | encode | 7,223,743 | 6,735,468 | -6.8% |
| `cas_run` | decode | 1,745,895 | 1,563,038 | -10.5% |
| `cas_ref_snap` | encode | 11,103,980 | 10,007,706 | -9.9% |
| `cas_ref_snap` | decode | 1,522,831 | 1,362,434 | -10.5% |
| `cas_part_manifest` | encode | 8,076,812 | 7,806,388 | -3.3% |
| `cas_part_manifest` | decode | 1,341,552 | 1,227,875 | -8.5% |
| `cas_fold_seal` | encode | 8,275,924 | 8,203,014 | -0.9% |
| `cas_fold_seal` | decode | 1,453,680 | 1,156,330 | -20.5% |
| `cas_ref_catalog` | encode | 14,946,922 | 14,125,533 | -5.5% |
| `cas_ref_catalog` | decode | 2,046,954 | 1,759,860 | -14.0% |

## Byte accounting {#byte-accounting}

Produced by the harness's own oracle (`--report_format_caps`) on both binaries, at n = 1000. The
maximum record count is found by binary search with the real encoder as the oracle, not computed
from a delta table.

| format | decompressed before | decompressed after | delta | stored after | max records under cap, before | after |
|---|---:|---:|---:|---:|---:|---:|
| `cas_run` | 94,103 | 101,442 | +7.8% | n/a | n/a | n/a |
| `cas_ref_snap` | 94,934 | 123,988 | +30.6% | 4,128 (zstd) | 660,207 | 514,053 |
| `cas_part_manifest` | 101,819 | 116,344 | +14.3% | 3,344 (zstd) | 2,622,258 | 2,298,073 |
| `cas_fold_seal` | 93,880 | 125,623 | +33.8% | n/a | 2,881,746 | 2,153,504 |
| `cas_ref_catalog` | 94,185 | 103,844 | +10.3% | n/a | 2,851,448 | 2,586,082 |

The `n/a` cells are not zeros and not omissions. `cas_run` is stored `PinnedRaw` and is never
compressed, and it has no object cap to search against because it is streamed one line at a time and
never materialized whole — the oracle prints that reason rather than a number. `cas_fold_seal` and
`cas_ref_catalog` are stored raw, so a "stored bytes" figure would be the decompressed one repeated.

**The stored-bytes column is a fixture artifact and must not be read as production compression.**
The synthetic fixtures repeat structure by index, so zstd collapses them far harder than real data
would: `cas_ref_snap` compresses 123,988 bytes to 4,128, a ratio of 30, where production rows carry
incompressible hashes. The column is kept because the campaign promised it, and labelled because the
number would otherwise mislead.

**The cap maxima are not affected by that artifact**, which was checked rather than assumed:
`openObject` enforces the object cap against declared decompressed size, and every reported maximum
matches the cap divided by decompressed bytes per record to within 5%. So the maxima mean "records
whose decompressed encoding fits the cap", they are reproducible, and the drops — `cas_ref_snap` from
660,207 to 514,053, `cas_fold_seal` from 2,881,746 to 2,153,504 — are the byte growth showing up as
capacity, at 78% and 75% of the previous ceiling. Both remain orders of magnitude above any real
object.

## What the numbers mean {#what-the-numbers-mean}

**Encode is close to free.** The median encode delta is about +4%, and the per-case spread runs from
−1.4% to +11.0% with the two passes disagreeing by up to 9 points on individual small cases. That
spread is noise, not signal: encode deltas sit near zero, so relative scatter is large. The honest
statement is that encoding costs a few percent and the measurement cannot resolve it more finely than
that.

**Decode is where the cost is, and it is proportional to the bytes.** This is the table that decides
the campaign's central question, comparing each format's decode-time growth against its own byte
growth:

| format | bytes | decode time (median over n) | decode minus bytes |
|---|---:|---:|---:|
| `cas_run` | +7.8% | +12.8% | **+5.0 points** |
| `cas_ref_snap` | +30.6% | +14.3% | −16.3 points |
| `cas_part_manifest` | +14.3% | +8.3% | −6.0 points |
| `cas_fold_seal` | +33.8% | +26.8% | −7.0 points |
| `cas_ref_catalog` | +10.3% | +14.1% | **+3.8 points** |

Three of the five formats decode *more cheaply per byte* after the cut than before it. The alarming
`cas_fold_seal` figure is the clearest case: its rows carry the most keys and the shortest values, so
key text dominates and its encoded size grew by a third — and its decoder absorbed that growth at a
discount. The design's claim that the dominant performance risk is the longer keys themselves is what
these five rows show.

**Two formats exceed their bytes, by single-digit margins.** `cas_run` decode costs about 5 points
more than its byte growth explains, and `cas_ref_catalog` about 4. That residue is the open question,
and it is precisely the one the design already anticipated: the spec requires that the enum tables and
the match helpers add no call, allocation, or branch beyond the comparisons the pre-cut chains made,
and it names the `cas_run` marker word and `cas_run` token matching as the first things to inspect.
The assembly review is the instrument for that, and it now has a specific number to explain rather
than a general survey to perform.

There is a plausible mechanism to test it against, stated here so the review can refute it rather
than rediscover it. `JsonObjectReader::nextKey` rejects duplicate keys with a linear scan over a
`std::vector<String>` of the keys already seen, comparing whole strings — quadratic in the number of
keys on a row. The cut made every one of those comparisons longer, and it also made them share
prefixes by design (`snap_generation`, `snap_attempt`, `snap_pruned_through`), so each comparison now
runs further before it can differ. That predicts a per-row penalty concentrated on decode, which is
what the shape of these results shows. It does not by itself predict why `cas_run` should exceed its
bytes while `cas_fold_seal` does not, which is what makes it a hypothesis rather than a conclusion.

## What is not covered {#what-is-not-covered}

- **Only five formats.** The other twelve are not measured. They were chosen because the design names
  them, but a format with an unusual key-to-value ratio could behave differently.
- **Synthetic fixtures.** Field widths were chosen to resemble production and the workloads are
  documented in the harness, but no figure here comes from a real pool. The stored-bytes column is
  the one place where that gap is large enough to invalidate the number outright.
- **No absolute claim.** `cpu_scaling_enabled` is true throughout; these are relative medians on one
  machine, not portable throughputs.
- **No CI assertion.** By the design's own instruction, none of this becomes a timing test. It is
  review evidence, and it expires if production code changes — the freeze commit is named above for
  exactly that reason.
