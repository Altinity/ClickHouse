---
description: 'How the CAS cas_run decode path was profiled and what made it faster, including the two hypotheses the measurements refuted.'
sidebar_label: 'CAS decode optimisation'
sidebar_position: 100
slug: /superpowers/cas/decode-optimisation
title: 'CAS decode — profiling and what actually helped'
doc_type: 'reference'
---

# CAS decode — profiling and what actually helped {#cas-decode-profiling-and-what-actually-helped}

The wire-key measurement left a question worth answering separately from the campaign: the decode
path costs what it costs, and some of that cost predates the cut entirely. This is what profiling it
found, including the two things it proved were **not** the problem.

## Two hypotheses, both refuted before anything was written {#two-refuted-hypotheses}

**The duplicate-key scan.** `JsonObjectReader::nextKey` rejects duplicate keys with a linear
`std::find` over a `std::vector<String>`, comparing whole strings — quadratic in the keys on a row.
The semantic rename made every one of those comparisons longer and gave the names shared prefixes
(`snap_generation`, `snap_attempt`, `snap_pruned_through`), so each comparison runs further before it
can differ. This was the obvious suspect and it is wrong twice over. It predicts that the format with
the most keys per row suffers most; that format is `cas_fold_seal`, and it decodes more cheaply per
byte than any other. And the profile puts the scan at **0.17%** of decode instructions.

**The line reader.** `readLine` built its result one character at a time with `push_back`, re-checking
the length cap on every character, and returned a `String` by value — an allocation per row, for rows
hundreds of bytes long. Replacing it with a buffer scan plus a bulk append, and giving the stream
reader a reusable scratch, is a strict improvement and it measured **zero**: every format moved less
than 2%, which is this harness's noise floor. The change was kept because it is better code, not
because it bought anything.

Two guesses from reading code, two refutations from measuring it. The third attempt waited for a
profile.

## The profile, and the mistake in the first one {#the-profile}

`perf` was unavailable (`perf_event_paranoid` is 4 and raising it is a system-wide change), so the
profile is callgrind's instruction counts — deterministic, privilege-free, and adequate for the
question "where does the work go".

**The first profile was discarded**, and for the same reason a measurement was discarded earlier in
this campaign: it answered a different question than the one asked. Callgrind counts the whole
process, and a decode benchmark builds its input by *encoding* it first, so fixture construction and
the encoder appeared in what claimed to be a decode profile. Re-running with
`--collect-atstart=no --toggle-collect='*SourceEdgeRunReader*next*'` confines the count to the decoder.

What the clean profile shows, inside `SourceEdgeRunReader::next` only:

| | share of instructions |
|---|---:|
| `MemoryTracker::allocImpl` | 5.86% |
| `MemoryTracker::free` | 4.17% |
| `std::string::append` | 4.16% |
| `ProfileEvents::increment` | 3.96% |
| `CurrentMemoryTracker::allocImpl` | 2.83% |
| jemalloc `rtree_read` | 2.75% |
| `CurrentMemoryTracker::free` | 2.61% |
| `DigestCodec::fromHex` | 3.18% |
| `readJSONStringInto` (with its `find_symbols`) | 5.55% |

**About a fifth of every instruction executed inside the decoder is allocation accounting.** Not
`malloc` — the accounting around it. Each allocation in ClickHouse passes through
`CurrentMemoryTracker`, `MemoryTracker::allocImpl` and a `ProfileEvents` counter, and that chain is
what the decode loop was paying, once per row, several times over.

## What allocated, and the fix {#the-fix}

`JsonObjectReader` was constructed **per row**. Its `std::vector<String>` of seen keys therefore
allocated per row, and `readHex128` built a 32-character `String` — past the small-string threshold,
so a heap allocation — for every hex field, only to parse it and throw it away.

The fix is to stop rebuilding what can be reused:

- `JsonObjectReader::reset` re-points an existing reader at another object, resetting the object-level
  state in full — the key set and the position — while keeping the buffers it has already grown.
- A reader-owned scratch serves values that are parsed and discarded, so a hex digest or a decimal
  counter costs no allocation once the scratch has reached its size.
- `SourceEdgeRunReader` holds one reader and one line scratch for the whole run.

The reader is shared by all seventeen formats' decoders, so correctness came before speed: the
change was gated on the unit battery, 2250 tests, before any number was taken. A reused reader must
accept and reject exactly what a fresh one does, and the duplicate-key and strict-format tests are
what would catch an incomplete reset.

## What it bought {#what-it-bought}

A/B on one machine: the same tree built twice, once with these changes reverted and once with them
applied, each measured at a 1-minute load under 0.8 (0.49 and 0.76). Provenance was checked by
content on both sides — the baseline binary does not contain `readLineInto`, the optimised one does.

| format | before | after | change | records/s |
|---|---:|---:|---:|---|
| `cas_ref_catalog` | 55.95 ms | 10.68 ms | **−80.9%** | 1.79M → 9.36M |
| `cas_run` | 63.98 ms | 19.71 ms | **−69.2%** | 1.56M → 5.07M |
| `cas_ref_snap` | 73.95 ms | 22.66 ms | **−68.6%** | 1.35M → 4.41M |
| `cas_fold_seal` | 86.52 ms | 34.75 ms | **−62.5%** | 1.16M → 2.88M |
| `cas_part_manifest` | 81.21 ms | 34.25 ms | **−58.0%** | 1.23M → 2.92M |

**Decoding is 2.4 to 5.2 times the throughput it was**, on every format the harness covers. Two more
row loops were given the same treatment — `cas_gc_outcomes` and `cas_ref_log` — and are not measured,
because the harness does not cover them; the mechanism is identical but the number is not claimed.

### An earlier number in this document was wrong, and wrong in the flattering direction {#a-retracted-number}

A previous revision reported −42.4% for `cas_run` and zero for the `readLine` change. Both were
measured against a baseline file that a **stale background script had silently overwritten** with a
run of the already-optimised binary. The comparison was therefore partly against itself. The commit
that introduced these changes carries the −42.4% figure in its message; it is superseded by the
table above.

Two things make the corrected figures trustworthy where the retracted ones were not. The baseline
here reproduces the campaign's independently committed measurement to within 0.5% — 63.98 ms against
64.30 ms for `cas_run` — despite being taken hours apart under different load. And the improvement
appears on all five formats, which is what the mechanism predicts, rather than on one.

The `readLine` change's real contribution is still not separated from the reader change's, because
they were measured together after the contaminated comparison was discovered. It is not claimed to
buy anything on its own.

## A third attempt, measured and reverted {#a-third-attempt-reverted}

With the per-row allocations gone, a second profile showed the cost had moved where it should have:
allocation accounting fell from about 22% of decode instructions to 5.6%, and what dominated instead
was reading JSON strings by copying them — for `cas_ref_catalog`, `std::string::append`,
`readJSONStringInto`, its symbol scan and `memcpy` together came to roughly 36% of the decode.

So the next change read a JSON string **without copying it**: when the whole quoted run is already in
the buffer and carries no escape — which is every value this format writes, and always true for a row
decoder parsing from a buffer that holds the entire line — return a view instead of a copy, falling
back to the copying path otherwise so that no input changes meaning. It was wired into `readHex128`
and `readU64String`. The unit battery stayed green at 2250.

**It was reverted, because it made `cas_ref_catalog` slower and the reason is not understood.**

| format | run 1 | run 2 |
|---|---:|---:|
| `cas_ref_catalog` | **+7.5%** | **+6.0%** |
| `cas_fold_seal` | −4.9% | −3.9% |

Two independent runs, five repetitions each, on a quieter machine than the baseline they are compared
against — so load does not explain a slowdown. The other three formats moved less than 1.5%, inside
this harness's noise.

Part of the outcome has an explanation and part does not. The absent *gain* does: the fast path was
wired into two value readers, while most of the copying `cas_ref_catalog` does is in keys and string
values, which go through `nextKey` and `readString` and were left untouched. The measurement
therefore did not test the thing the profile pointed at. But nothing in that explains a *regression*
from removing a copy, and shipping a change whose effect cannot be accounted for — negative, on the
format the profile said would gain most — is worse than shipping nothing.

**Four predictions, three refuted.** The duplicate-key scan (0.17% of instructions), the
byte-at-a-time line reader (exactly zero), and the copy-free string read (a reproducible regression
where the largest gain was expected). The one that held — reusing the reader — was the only one made
*after* profiling rather than from reading the code. Reading code reliably shows what looks wasteful
and unreliably shows what is expensive; cost comes from how often something runs and how it touches
memory, neither of which is visible in the shape of the source.

## What this costs the campaign {#what-this-costs}

This is production code, so it moves the wire-keys campaign's freeze commit. Every measurement, lane
run, soak scenario and disassembly taken against the previous freeze describes a binary that no
longer exists, and has to be retaken. That price was accepted deliberately: the campaign's remaining
work already required production changes for three unmet acceptance criteria, so the freeze moves
once for all of them rather than once each.
