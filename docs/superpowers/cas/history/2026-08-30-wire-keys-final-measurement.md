---
description: 'What the CAS wire-key rename cost, what the decode optimisation bought, and where the two together leave decoding.'
sidebar_label: 'Wire keys final measurement'
sidebar_position: 98
slug: /superpowers/cas/wire-keys-final-measurement
title: 'CAS wire keys — the cut, the optimisation, and the net'
doc_type: 'reference'
---

# CAS wire keys — the cut, the optimisation, and the net {#cas-wire-keys-the-cut-the-optimisation-and-the-net}

Three questions, answered separately because they have different answers and mixing them would
flatter the change:

1. What did renaming the keys cost?
2. What did the decode optimisation done alongside it buy?
3. Where do the two together leave decoding, against where it started?

## The short answer {#the-short-answer}

**Longer keys grew the encoded objects by 7.8% to 33.8%** — the price of readable field names.

**That cost almost nothing to decode on four of the five formats** (+4.1% worst, and one is
negative), and cost `cas_fold_seal` 34.6%, which is exactly proportional to its byte growth. Encoding
is unaffected at +0.5%.

**The optimisation found while measuring makes decoding 57.6% to 80.7% faster**, and it is
independent of the rename: it removes a per-row reader rebuild that both spellings paid equally.

**Net, decoding is 53.6% to 78.7% faster than before the campaign started** — 2.2× to 4.7× the
throughput, with the more readable keys.

## The three measurements {#the-three-measurements}

Every column is the median over four record counts (100 to 100,000), from three repetitions, on one
machine under the load protocol.

| format | encoded bytes | cut alone | optimisation alone | net |
|---|---:|---:|---:|---:|
| `cas_run` | +7.8% | +4.1% | −69.1% | **−66.9%** |
| `cas_ref_snap` | +30.6% | +1.5% | −68.1% | **−64.3%** |
| `cas_part_manifest` | +14.3% | +0.7% | −57.6% | **−56.4%** |
| `cas_fold_seal` | +33.8% | +34.6% | −63.7% | **−53.6%** |
| `cas_ref_catalog` | +10.3% | −1.5% | −80.7% | **−78.7%** |

Negative is faster. The three columns are three different comparisons and each is measured, not
derived:

- **cut alone** compares pre-cut against post-cut with **both sides carrying the optimisation**, so
  the only difference is the key spellings.
- **optimisation alone** compares pre-optimisation against post-optimisation on the **post-cut** code,
  so the only difference is the reader reuse.
- **net** compares the state before the campaign — pre-cut, pre-optimisation — against the state
  after both.

That is a complete two-by-two: four builds, each measured, no figure computed by subtracting one
percentage from another.

## Reading it {#reading-it}

**The rename is nearly free to decode except where key text dominates a row.** Four formats absorb a
byte growth of up to 30% for essentially no time. `cas_fold_seal` does not, and the reason is
structural rather than a defect: its rows carry the most keys and the shortest values, so key text is
the largest part of its bytes, and its decode tracks its bytes almost exactly — 34.6% against 33.8%,
a time-per-byte change of +0.6%. A workload that folds very large seals is the one place the rename
is visible, and there it costs about a fifth of decoded records per second.

**The optimisation is not a wire-key story and is not counted as one.** It removes a per-row rebuild
of the JSON object reader and its seen-key store, which both the old and the new spellings paid
identically. It is reported here because it landed during this campaign and because the net column
would be dishonest without naming where the speed came from.

**Encoding did not move.** +0.5% at the median across all five formats.

## Conditions {#conditions}

| | |
|---|---|
| Before side | worktree at `65ec8688cdb`, with the optimisation ported onto it for the cut-alone column |
| After side | freeze commit `3c948a46d1c` |
| Build configuration | verified identical — the driver diffs both `CMakeCache.txt` files and refuses to run when they differ |
| Provenance | checked by binary content, not mtime: both sides carry `readLineInto`; only the before side carries the pre-cut `!pse`, only the after side the post-cut `!prev_epoch` |
| Loads (1 min) | 0.49 / 1.00 for the pre-optimisation pair, 0.72 / 1.21 for the post-optimisation pair |
| Passes | forward and reverse; the two agree within 1 to 2 points on every format |

**A caveat the table cannot carry.** The cut-alone and optimisation-alone columns each come from one
matched pair measured back to back. The net column spans two different runs taken hours apart, so it
carries the combined variance of both. It is the least precise of the three, and the one to treat as
an order of magnitude rather than a figure to three digits.

## What is not covered {#what-is-not-covered}

- Five of seventeen formats — the ones the design names.
- Synthetic fixtures with production-plausible field widths, not data from a real pool.
- `cpu_scaling_enabled` is true; these are relative medians on one machine.
- No timing assertion enters CI, by the design's own instruction.
