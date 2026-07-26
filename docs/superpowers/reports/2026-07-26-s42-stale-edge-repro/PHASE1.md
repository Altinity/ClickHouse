# S42 stale-edge reproduction — systematic debugging, Phase 1

**Status: Phase 1 complete. Root cause NOT yet established. One hypothesis refuted, one specific
hypothesis standing and untested. No fix attempted — the Iron Law holds.**

Live reproduction preserved on the ca-soak stand as of 2026-07-27 ~22:20 UTC.

---

## 1. What reproduced {#what}

S42 (`--scale ci`, seed 43) on a QUIET host: `FAIL (26/28 verdicts pass)`.

```
fsck pre-restart : stale_edge == 0  ->  observed 12
fsck post-restart: stale_edge == 0  ->  observed 67
```

`stale_edge` = a blob whose every source edge names a manifest that no longer exists. Its in-degree is
pinned above zero forever, so the incremental GC can never reclaim it. Only `ca-gc-rebuild` can.

## 2. The run was clean — this is the product, not the environment {#clean}

```
other_failures            0      <- the environment did not interfere at all
QueryMemoryLimitExceeded  1965   <- injected faults DID fire; the run is not vacuous
CasRefApplyPoisoned       0      <- the critical invariant held
CasRefAppendWedged        0
CasGcUnmatchedRemoveDeltas 0     <- the `-1`-before-`+1` path did NOT fire
acked blocks              12998
```

Contrast with seed 42 on a loaded host: 23,561 environmental failures and `stale_edge == 0` on both
readings. **One run in two reproduces**, so this is timing-dependent, and — an inversion worth explaining —
the DIRTY run was clean while the QUIET one reproduced. Chaos may simply have kept the workload from
reaching the window.

## 3. The residue is PERMANENT, measured over 145 GC rounds {#permanent}

25 samples, one per minute, `ca-fsck --detail` plus a fingerprint of the stale-edge key set:

```
21:57  rounds=109  stale_edge=15  keyset=c82df19e  unreachable=15 pending_gc=0 awaiting_gc=0
   ... 23 identical samples ...
22:21  rounds=254  stale_edge=15  keyset=c82df19e  unreachable=15 pending_gc=0 awaiting_gc=0
```

**145 GC rounds passed and reclaimed none of them; the key set fingerprint never changed.** The sharpest
part is `pending_gc=0, awaiting_gc=0`: these blobs are not slow to drain, they are **never nominated**.
Same signature as the historical 56 that stayed flat across 1,062 rounds.

The earlier 12 → 67 → 15 churn was partly transient: 52 blobs did drain once GC caught up. **So the oracle
as measured mixes permanent stale edges with in-flight ones** — a fact about the ORACLE that matters for
how S42's verdict should be read.

## 4. Two of my own instruments were wrong before the data was {#instrument-errors}

- **Broken round counter.** I sampled `event_type='Round'`, which does not exist (`Start`/`Finish`/`Phase`).
  The first four flat readings could not be attributed to any number of rounds. Re-run against `Finish`.
- **`adds > removes` is a heuristic, not the truth.** In-degree is a SET with last-wins ordering, so
  counting events does not strictly identify residuals. The authoritative source is the in-degree run
  itself, which independently gives the same 15.

Recording both because they nearly produced confident nonsense.

## 5. The trace: what the audit log shows {#trace}

Per-blob, the CA log records every `root_add` (+1) and `root_remove` (−1) with `manifest_ref_instance`
(`epoch:build_sequence:ordinal`) and `path`. Pairing them isolates the unmatched adds:

```
ch128:0ba08f20…  1:34437:1  id.bin              adds=1 removes=0
ch128:3a111592…  1:34447:1  id.bin              adds=1 removes=0
ch128:9821469e…  4:66:1     id.bin              adds=1 removes=0
…
```

Two clusters:

- **epoch 1, build sequences ~9825 and 34437–34449** — during the fault-injection window;
- **epoch 4, sequences 66 and 71** — LOW numbers, i.e. after the leg-C restart minted a new writer epoch.
  This is where the 12 → 67 growth came from.

Following one manifest end to end: **`1:34437:1` emitted SIX `+1` edges in one instant** (`id.bin`,
`id.cmrk2`, `payload.bin`, `payload.cmrk2`, `payload.size.bin`, `payload.size.cmrk2` — a complete part file
set) **and never emitted a single `−1`.** The manifest is now gone. Only 2 of its 6 blobs are stale-edge;
the other 4 have other live references.

## 6. Hypothesis REFUTED: the dead-precommit skip {#refuted}

The intake loop has a path that deliberately skips a `-1`:

> "a precommit naming a build PROVABLY DEAD by the durable watermark floor … can never activate, and its
> body will never return. Skip it (non-activating, advance the log)"

Its justification — "no edge to mirror" — holds only if the matching `+1` was never folded. If it WAS
folded in an earlier round, skipping the `-1` strands it forever. That fits the signature exactly.

**Refuted by its own counter: `dead_precommits_skipped = 0` on both nodes.** The path never fired. Not
built upon.

## 7. Hypothesis STANDING, untested: abort → orphan sweep {#standing}

Manifest deletions split into exactly two paths:

```
114,945  "owner-removed manifest body; exact-token delete after decrements adopted"   <- normal, a -1 happened
     71  "orphan-manifest sweep: exact-token delete of an eligible+unowned build-prefix body"
```

And the build lifecycle under allocation faults shows a gap:

```
build_abort        130   "appended a precommit-removal event for the live precommit (body left for GC)"
precommit_removed   18
```

**130 aborts against 18 precommit removals.** The orphan sweep deletes a manifest body that is
"eligible + unowned" — it does not require that the manifest's edges were decremented, because an unowned
manifest should not HAVE folded edges.

**The hypothesis:** an allocation fault aborts a build whose precommit `+1` edges were ALREADY folded. The
abort leaves the body "for GC". The manifest becomes unowned, the orphan sweep deletes it, and the folded
`+1` edges are stranded with no `-1` ever emitted — permanently unreclaimable blobs whose manifests no
longer exist. Which is exactly the observed class.

**This is UNTESTED.** What would confirm it: correlate the 71 orphan-sweep deletions against the manifest
instances carrying unmatched adds. What would refute it: if the swept manifests are disjoint from the
stranded ones, the sweep is innocent and the `-1` must be going missing some other way — most likely a
listing hole in probe A's blind spot (a hole identical in BOTH enumerations, or a namespace dropped
wholesale, neither of which aborts folding).

Note probe A DID fire twice in this very run, on `ca_soak_ch2/store/a68/…`, same signature as before —
walk 1 returned `0x199d` and skipped `0x199b`/`0x199c`, both HEAD-verified `present`. Those firings aborted
folding, so they should not themselves have leaked; but they prove the listing defect was active during
this run.

## 8. What Phase 2 should do {#next}

1. Correlate the 71 orphan-sweep deletions against the manifest instances with unmatched adds. Confirms or
   refutes §7 outright.
2. If confirmed, find whether the abort path is SUPPOSED to emit `-1` for already-folded precommit edges,
   and why 130 aborts produced only 18 precommit removals.
3. If refuted, the remaining candidate is probe A's blind spots, and the test is to instrument the two
   shapes it cannot see.

**Do not skip to a fix.** The reducer is correct (a set cannot cancel what it never received); the defect
is upstream of it, and which upstream path is still unproven.
