---
description: 'What the GC fold read-ahead is worth against a backend with a fixed per-request latency, measured per phase, and the serial step it leaves behind in ref intake.'
sidebar_label: 'GC fold read-ahead measurement'
sidebar_position: 47
slug: /superpowers/worklogs/cas-gc-fold-read-ahead-measurement
title: 'GC fold read-ahead — what it is worth, and what it leaves serial'
doc_type: 'guide'
---

# GC fold read-ahead — what it is worth, and what it leaves serial {#cas-gc-fold-read-ahead-measurement}

## Why this is not the soak {#why-not-the-soak}

The intended measurement was two `ca-soak` runs of one scenario at `cas_gc_read_concurrency` 1 and 16.
The soak stand was in use by another session for the whole of this work, and taking it would have
destroyed a running experiment, so this is a bounded substitute run entirely in the unit-test harness:
a `CountingBackend` subclass that sleeps a fixed interval on every `read` and every `head`, armed only
for the round under test. It answers "does the mechanism convert latency into parallelism, and where",
which is the question the design turns on. It does not answer "what does a real pool do", because the
scenario is three namespaces and about eighty ref logs while a real pool has thousands of namespaces.
Treat every ratio below as a property of the mechanism, not a forecast.

The delay is applied to `head` as well as `read`. A first version delayed only `read` and reported the
reduce phase as flat no matter what it did, because the zero-in-degree observation is a `HEAD`.

## Numbers {#numbers}

One GC round, same workload, `gc_read_concurrency` 1 against 8. Phase figures are the phase rows' own
durations; the round figure is wall time around `runRegularRound`.

| Delay per request | Phase / total | Concurrency 1 | Concurrency 8 | Ratio |
|---|---|---|---|---|
| 1000 µs | `fold_ref_intake` | 144 ms | 60 ms | 2.40 |
| 1000 µs | `fold_reduce` | 90 ms | 75 ms | 1.20 |
| 1000 µs | whole round | 247 ms | 150 ms | 1.65 |
| 200 µs | `fold_ref_intake` | 34 ms | 16 ms | 2.13 |
| 200 µs | `fold_reduce` | 21 ms | 18 ms | 1.17 |
| 200 µs | whole round | 59 ms | 37 ms | 1.59 |

Concurrency 16 was measured too and is within noise of 8, at both delays. Nothing in the round is
waiting on pool threads.

## Why intake stops at about 2.4 and not at 8 {#intake-ceiling}

Counting the round's reads by key class settles it. The round issues **83 ref-log GETs and 83 manifest
GETs — exactly one to one** — plus 66 reads of everything else.

The ref logs parallelise: their keys are arithmetic, so the lookahead knows the next thirty-two before
reading any of them. The manifest bodies do not, and cannot under this design. A log's manifest edges
are named by the log's own decoded body, so the earliest moment the round can know manifest `N`'s key
is after log `N` has been read and decoded. Every log in this workload names exactly one edge, so
hinting "all the edges of this log" hints one key and overlaps nothing with itself. What the manifest
read does overlap with is the ref-log reads still in flight for later positions, which is where the
part of the win above `2x` comes from.

So intake's remaining serial chain is one manifest round trip per log. That is the next thing to
attack, and it needs a different mechanism than a key-arithmetic lookahead: decoding an
already-fetched later log purely to learn its manifest keys, without moving the fold's own decode or
any decision. Recorded as `[gc-intake-manifest-edge-serial-chain]`.

## Why reduce moves so little {#reduce-ceiling}

`fold_reduce` gains 1.2. Its zero-in-degree `HEAD`s are read ahead, but they are not what the phase
spends its time on in this workload: the graduation gate's per-entry meta re-check is a `read` issued
inline from the merge, and it is not hinted, for the reason recorded in
`[gc-reduce-confirm-marker-read-ahead]` — its candidates are known only from the prior run's condemned
rows, which the merge streams. This measurement is the evidence that entry is worth doing: it is now
the dominant serial cost of the phase.

## Reproducing {#reproducing}

The benchmark was a temporary test appended to `src/Disks/tests/gtest_cas_gc_read_ahead.cpp` and was
NOT committed: a wall-clock assertion does not belong in the gate. It reused the suite's own `populate`
and `Gc` fixtures, added a `CountingBackend` subclass sleeping a fixed interval in `read` and `head`,
armed after `populate` and before `runRegularRound`, and reported each phase row's duration plus a
count of the backend's GETs bucketed by key substring (`/_log/`, `/cas/manifests/`, everything else).
Rebuild it from that description in fifteen minutes; do not resurrect it as a gate test.

The soak comparison this substitutes for is still owed, and is the number to quote for a real pool. Run
it when the stand is free: one scenario, phase 3 with `--duration`, once with
`<cas_gc_read_concurrency>1</cas_gc_read_concurrency>` in both storage configs and once without, then
compare `sum(phase_duration_microseconds)` per phase from each run's `gc_log` and read
`CASGCReadAheadHit`, `CASGCReadAheadMiss` and `CASGCReadAheadWasted` off the same rows.
