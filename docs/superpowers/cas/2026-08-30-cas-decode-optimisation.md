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

Measured on a quiet machine (1-minute load 0.37) against the same freeze-commit binary the campaign's
throughput tables came from, same flags, same fixtures.

| benchmark | before | after | change |
|---|---:|---:|---:|
| **`cas_run` decode** | 34.31 ms | 19.66 ms | **−42.4%** |
| `cas_ref_snap` decode | 42.08 ms | 41.91 ms | −0.6% |
| `cas_part_manifest` decode | 53.71 ms | 52.32 ms | −2.2% |
| `cas_fold_seal` decode | 57.46 ms | 56.75 ms | −1.6% |
| `cas_ref_catalog` decode | 29.74 ms | 29.55 ms | −0.8% |
| encode, all five | | | −0.7% to +1.6% |

At 100,000 records, `cas_run` decode goes from **2.91 to 5.09 million records per second — a 75%
increase in throughput**.

**The shape of the result is the evidence that the mechanism is understood, not the size of it.** The
prediction written down before the run was: `cas_run` decode improves substantially, the other four
formats do not, and encode does not move — because only `cas_run` streams through the reusable
reader. That is exactly what came back. A number that large arriving on the wrong benchmarks would
have meant something else was going on.

Two caveats on the size. The gain exceeds the 22% of instructions the profile attributes to
allocation accounting, which is expected: instruction counts do not see cache misses, and allocation
churn costs memory-system time that callgrind cannot charge to it. And the first run of this
measurement was taken while the machine was still busy from the unit gate — it reported −43.2%, and
the quiet re-run reports −42.4%, so the load was not what produced the number.

**The other four formats are unimproved and that is not a failure — it is scope.** They decode whole
objects through a reader built once per object, so there is no per-row rebuild to remove. The same
treatment would only pay for a format that streams many objects through one call, and `cas_run` is
the only one that does.

## What this costs the campaign {#what-this-costs}

This is production code, so it moves the wire-keys campaign's freeze commit. Every measurement, lane
run, soak scenario and disassembly taken against the previous freeze describes a binary that no
longer exists, and has to be retaken. That price was accepted deliberately: the campaign's remaining
work already required production changes for three unmet acceptance criteria, so the freeze moves
once for all of them rather than once each.
