---
description: Design spec for remediating the CA GC S1–S4 review — Tier 1 (correctness: generation accounting, fail-closed session coverage, the data race, stale lock contracts) + Tier 2 (G1/G3: lock-free GcLogWriter I/O, sealed-tombstone index) + real lockless-path oracles. Tier 1 must precede Tier 2 because removing the full re-validate scan is what finally makes the lockless layer load-bearing for safety.
sidebar_label: 'CAS GC S4 review remediation'
sidebar_position: 14
slug: /superpowers/specs/ca-gc-s4-review-remediation
title: 'Content-Addressed MergeTree — GC S4 Review Remediation'
doc_type: 'guide'
---

# Content-Addressed MergeTree — GC S4 Review Remediation {#ca-gc-s4-remediation}

**Status:** design spec, awaiting review. **Date:** 2026-06-05. **Backlog:** B69 (the S4
DO-NOT-MERGE-UNTIL-PROVEN attended-review gate — this remediation is its resolution path). **Branch:**
`cas-mergetree-poc`. **Inputs:** the GC convergence spec (`2026-06-04-ca-gc-convergence-design.md`) and the
attended review of S1–S4 (`7bc7f66e869a..07a64679939`).

## 1. Goal and scope {#goal}

Make the lockless S4 GC path **correct** and **prove it with tests that exercise the real path**, then
restore the **G1/G3** goals the implementation currently undercuts. The review's "minimum before S4 ships"
plus the two goal-defeating majors, with the remaining findings converted to backlog items.

**In scope:** Tier 1 (correctness: #1, #6, #2, #5, #7) + Tier 2 (G1/G3: #3, #4, plus two near-free fold-ins)
+ the real lockless-path oracles. **Out of scope (→ backlog):** the minors, diagnostics, and the two
remaining needs-verify items (§7).

## 2. Framing correction — why these are Tier 1 (verified) {#framing}

**The sweep's current safety net is a generation-BLIND full reachability re-validate scan.**
`ContentAddressedGC::sweepCandidates` gates every delete on `identity_reachable_in` (`ContentAddressedGC.cpp:450`),
which builds the reachable set from `markReachableBlobs` over `listLivePartIds` (a full scan of
`store/.../refs/` + resolve every live manifest) and checks the candidate's **bare identity** `blobKey(H)`
(g=0), never the generationed key (`:454-458`, `:880-881`). A candidate at any generation `(H,g)` is therefore
spared from sweep iff any live part still references bare `H` — it **over-protects every generation** of any
still-referenced content. The reconciliation collector uses the same identity-level check (`:880`), so it
over-protects too.

**Consequences (this corrects the review's own severity wording on #1/#2):**
- In the **current** code, **#1 and #2 are NOT data-loss.** A live part's blobs are protected by the
  ref-scan re-validate regardless of whether the `+` carried the right generation (#1) or whether the
  `+`/session existed at all (#2) — the published ref alone makes bare `H` reachable. So **#1 is a guaranteed
  leak of resurrected generations + the S3 reclamation feature is inert** (counts always key `g=0`); **#2 is
  log-drift/leak, not a swept-live-blob.**
- They become **data-loss only once that full re-validate scan is removed** to actually achieve G3 (when the
  compaction count becomes the *sole* delete authority). Right now the §5.1/§6.2/§7 lockless machinery is
  **not yet load-bearing for safety** — the brute-force reachability scan silently is.
- **Therefore Tier 1 MUST precede Tier 2.** The reason #1/#2 are Tier 1 is *"make the lockless layer
  trustworthy before you lean on it,"* not *"stop a sweep happening today."* Tier 2 (#4, which replaces the
  full scan) is the step that finally makes the compaction count authoritative — and must not land until
  Tier 1 is solid and the new oracles prove it.
- **Do NOT make the re-validate scan generation-aware** — it is intentionally identity-level / over-protective
  (the safety net). Narrowing it would *remove* the net. (The review's "needs-verify: make it
  generation-aware" is retracted: blind-by-design, safe.)

## 3. Tier 1 — correctness {#tier1}

### 3.1 #1 (blocker) — generation drop in `splitDeltaByShard` {#fix1}
`GcLogWriter::splitDeltaByShard` (`GcLogWriter.cpp`) initializes only `op`/`event_id`/`part_id`, pushes each
`pin` but never `delta.pin_generations[i]`, and sets `carries_part_edge=true` without copying
`delta.manifest_generation` — so every `gc/log` fragment serializes at generation 0 and `GcCompaction` keys
everything `CountKey{Blob,H,0}`. **Fix:** in the pin loop push the paired `delta.pin_generations[i]` (default
0 if absent) into the same shard fragment; copy `delta.manifest_generation` onto the part-edge fragment
(consume the now-load-bearing `carries_part_edge`). Carry the generations through serialize→fold so the
compaction keys `CountKey{Blob,H,g}` / `CountKey{Part,id,mg}` at the real generation.

### 3.2 #6 (coupled to #1) — drop-path settled generation via the ref sidecar {#fix6}
After #1, a `-` keyed at `active`'s *current* generation will not net against a `+` settled at a different
`g` if a resurrection intervened (the old generation's count stays >0 → snapshot orphan). **Fix:** at commit,
record the **settled `(H,g)` pinset and `mg`** in the part's existing **`.meta` bundle sidecar**
(`refMetaKey`, already written at commit and read at drop — no new object, no extra PUT). The drop reads the
sidecar and emits the `-` at the matching generation, so `+`/`-` net even across an intervening resurrection.
Per-part is exactly right: re-deriving from `active` or re-resolving would give the wrong (current)
generation.

### 3.3 #2 (blocker) — fail-closed session coverage on `+`-flush failure {#fix2}
Today a throw from `appendAndFlushForCommit` (`ContentAddressedTransaction.cpp:~1573-1610`) is swallowed,
`settled_delta_epochs` stays empty, `allSettledEpochsFolded({})` returns true (`:~1727`), and the covering
session is released immediately after the ref is published — leaving the reference covered by neither the log
nor a session. (Leak today; **data-loss once Tier 2 removes the full scan** — exactly the gap S4 forbids.)
**Fix — three explicit session states, retain-not-abort:**
1. **legit-no-deltas** (nothing to log) → release now.
2. **deltas-folded** (`isEpochFolded` confirms every settled epoch) → release.
3. **deltas-failed** (the `+`-flush threw) → **sticky**: the session is retained AND **exempt from
   lease-expiry reaping** until its `+` is durably re-logged and folded. (Critical: without the reaping
   exemption, the 300 s lease reaps it and — since reconciliation defaults to off — nothing rebuilds the
   `+`, re-opening the gap.)
**Recovery is re-log-retry, not reconciliation** (reconciliation is off by default — depending on it is
depending on a non-path): on a `+`-flush throw, retry the append (idempotent by `event_id`) on a bounded
background path; release the sticky session only when `isEpochFolded` confirms the re-logged `+` landed.
**Also:** `flushBufferLocked` must clear `buffer.fragments` **before** the throwing write, so a failed flush
leaves no moved-from zombie fragments that the next flush serializes as junk `gc/log` entries.

### 3.4 #5 (major, safety) — unlocked `in_flight_pinned_blobs` read {#fix5}
`collectReconciliationCandidates` reads the shared `in_flight_pinned_blobs` `std::set` unlocked
(`ContentAddressedGC.cpp:~883`) while `commit` mutates it — UB (dormant: reconciliation cadence defaults 0).
**Fix:** pass the `pinned_snapshot` into the collector and read from it, exactly as `sweepCandidates`
already does.

### 3.5 #7 (major) — stale `*Locked` contracts/names {#fix7}
`sweepCandidatesLocked`, `collectSealedTombstoneCandidatesLocked`, `collectReconciliationCandidatesLocked`
(`ContentAddressedGC.cpp:368,613,839`) document a "MUST hold `gc_lock`" precondition S4 deliberately removed —
a maintenance time-bomb, and how #5 slipped in. **Fix:** drop the `Locked` suffix; rewrite the doc-contracts
to the S4 lock-free convention + the `pinned_snapshot` parameter.

## 4. Tier 2 — restore G1/G3 {#tier2}

### 4.1 #3 (major, G1) — `GcLogWriter` mutex held across S3 I/O {#fix3}
`enqueue`/`appendAndFlushForCommit`/`reappendIfAdvanced` hold `mtx` across `readShardEpoch` (HEAD+GET) and
`flushBufferLocked` (PUT), the re-append looping up to 8× under the lock — and `GcLogWriter` is one per-pool
instance shared by all committers, so every concurrent commit blocks all others for multiple ~300 ms
round-trips (the opposite of G1). **Fix:** take `mtx` only to move fragments in/out of the buffer; do the S3
I/O (and the re-append re-reads) **outside** the lock; cache the per-shard epoch (it changes once per GC
round, refreshed on a fold).
**Fold-ins (same `GcLogWriter`/commit code — avoid re-opening these files later):**
- **shutdown → `flushAll`:** `ContentAddressedMetadataStorage::shutdown` must `flushAll()` the writer;
  buffered `-` deltas are otherwise silently lost (over-count/leak).
- **double `persistSession`:** `commit` calls `persistSession` twice with no intervening state change — one
  extra hot-path PUT (directly against G1). Remove one.

### 4.2 #4 (major, G3) — full bucket scan every sweep round {#fix4}
`collectSealedTombstoneCandidates` (`runSweepOnce:806`) LISTs the entire `blobs/`+`parts/` tree every round
(`:649-650`) to re-present sealed-but-unswept tombstones — so the "0 LISTs on the normal compaction path"
claim is false at scale, and (per §2) this scan is currently the de-facto safety net. **Fix:** maintain a
compact **`gc/sealed/<shard>`** index of open tombstones (the seal step adds an entry; sweep and recover
remove it); the sweep LISTs only that index, not the whole tree. **This is the step that removes the
generation-blind full scan as the implicit safety net — so it MUST land after Tier 1 + the new oracles
prove the §5.1/§6.2/§7 lockless layer is the real, generation-correct authority.**

## 5. Testing — close the lockless-path gap (the gate) {#testing}

The current §7 oracles run with `reconciliationCadenceRounds=1`, so candidates come from the *reconciliation
fallback*, not the compaction/lockless path; the safety claims are unproven. The new oracles are the gate
that Tier 1 is solid before Tier 2 removes the scan. All deterministic, NO sleeps:
1. **writer→log→compaction generation** — resurrect a blob to `g=1`, commit through the **real**
   `GcLogWriter` (not `makeDelta`), fold, assert `CountKey{Blob,H,1}` appears and nets to zero on drop (would
   have caught #1); a `(part_id,mg>0)` variant for the manifest edge.
2. **real lockless interleaving** (`reconciliationCadence=0`, `gc_lock` dropped, two threads) — a `+` lands as
   its epoch closes/folds → the blob survives (re-append + dedup); a committed-but-unfolded **sticky** session
   protects a blob whose ref was dropped.
3. **`reappendIfAdvanced` actually fires** — enqueue into E, externally `compactShard` to close E→E+1, then
   `flushAll`; assert the delta is re-logged and folds with the correct (deduped) count.
4. **fault-injection (#2)** — throw from the `+`-flush; assert the session is NOT released and becomes
   sticky-not-reaped, then the bounded re-log lands the `+` and the session releases on fold.
5. **negative codec tests** for `GcLogBatch`/snapshot (bad magic / bumped version / bad op), mirroring the
   `WriteSession` ones.
6. **`gc/sealed/<shard>` index (#4)** — seal adds, sweep/recover removes; a normal sweep round issues **0**
   `blobs/`+`parts/` LISTs (assert the op counter); a sealed-but-unswept tombstone is still re-presented
   across rounds via the index.
Plus: the full `ContentAddressed*` gtest suite + the CA-default smoke (esp. `04279_content_addressed_gc`)
stay green at every step; non-CA regression unchanged.

## 6. Sequencing & safety {#sequencing}
**Tier 1 first** (#1+#6 generation correctness, #2 fail-closed session, #5 race, #7 contracts) + the new
oracles 1–5 → this makes the lockless layer generation-correct and proven. **Then Tier 2** (#3 lock-free I/O +
fold-ins, #4 sealed-index + oracle 6) → restores G1/G3 and removes the full-scan safety net. The ordering is
**load-bearing**: #4 removes the generation-blind re-validate scan that is currently the real safety net, so
it must not land until Tier 1 is solid and oracles 1–4 prove the §5.1/§6.2/§7 machinery is the correct
authority. The `gc_lock` stays dropped (S4) throughout — these are fixes *on* the lockless path. After this
remediation + oracles pass, the B69 attended-review gate can be re-evaluated for sign-off.

## 7. Out of scope → backlog {#backlog}
Convert to tracked items (NOT in this spec): observability counters
(`cas_s3_sequential_control_depth_per_commit`, `cas_writer_tombstone_s3_fallbacks`, `cas_log_batch_size`,
`cas_s3_session_fallbacks`) + S1-drift `LOG_WARNING`+counter + sweep/reaper log levels; `GcCompaction`
spill-or-correct-the-doc; `allSettledEpochsFolded` per-commit `gc/snap` LIST (cache the watermark or drop the
eager-reap); dead code (`flushDueWindows`, `enqueue` return value, `CompactionResult::folded_counts`, S1-drift
gating); robustness nits (generation-parser overflow guards, epoch-wrap `chassert`, atomic
`reconciliation_cadence`); diagnostics (object keys in corrupt-codec exceptions, the misleading
resurrection-cap message, a version-conditional `WriteSession` deserialize like `PoolMeta`'s); and the two
remaining needs-verify items — **session-lease-vs-large-uploads** (couples to #2's session-state work) and
the **`resolveAndResurrectGeneration` presence-check** (low-probability, masked by the read-path repair).
**Closed (answered, not tracked):** "data-loss-reachability under reconciliation" — the reconciliation check
is identity-level (`:880`), over-protective, no data loss either way (§2).

## 8. Risks {#risks}
- **#2's sticky session + bounded re-log** is the subtlest piece: it must be exempt from lease-reaping while
  undurable, retry idempotently by `event_id`, and release only on `isEpochFolded` — proven by oracle 4.
- **#4's sealed-index** must stay consistent with the actual tombstones (seal adds, sweep/recover removes,
  crash-idempotent); a missed entry re-introduces a leak (not data-loss — the tombstone object is still the
  durable truth; the index is an accelerator). Oracle 6 + a reconciliation cross-check bound it.
- **Tier-ordering discipline:** #4 must not merge before Tier 1 + oracles 1–4 are green (the load-bearing
  coupling, §2/§6).
