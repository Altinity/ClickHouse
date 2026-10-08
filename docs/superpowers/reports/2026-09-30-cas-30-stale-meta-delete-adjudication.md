> Adjudication of the stale `.meta` delete race (backlog CAS-30), written 2026-09-30 by a static read of `cas/gc-request-cuts` @ee3d945f380 and `antalya-26.6` @61d24118251. Nothing was built, run or model-checked. "A0" is the carried `marker_confirmed` change of `docs/superpowers/specs/2026-09-29-cas-gc-request-cuts-design.md` §2.

# A0 adjudication: a stale meta-delete job removes a later generation's condemn marker

Code: `gc-request-cuts` @ ee3d945f380, base 61d24118251. Paths are relative to
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`. Static reading only, nothing run.

## Verdict

- **Branch: REAL.** The schedule is reachable. The end state is a dangling reference, which means data loss.
- **Base: REAL, and it existed before this branch.** The same schedule works on the base with no restart. On the
  base it is also wider: it needs no graduation-budget exhaustion. A0 adds only the restart / leader-change
  variant, where the budget carries the row.
- **Cause:** the defect is in `deleteConfirmedMeta`, not in A0. It deletes whatever meta is current. So the
  spec §2 premise ("the marker leaves `Condemned` only after T is displaced or deleted") is false for a
  job of a deposed leader. Dropping A0 does not fix it.
- **Format change: none needed.** `BlobMeta::condemn_round` and `RetiredEntry::condemn_round` already exist.

## Decisive code facts

1. `Gc/CasGc.cpp:466-468`: `applyRedeleteOutcome` schedules `scheduleConfirmedMetaDelete(entry.ref)` on
   `Removed`/`Gone`. It passes only the `BlobRef`. It runs in phase 11 `pending_deletes`, pre-CAS.
2. `Gc/CasGcMetaWriter.cpp:160-167, 86-92`: the job admits its own operation on `openRequests()`. It runs
   `loadMeta` and then `deleteMetaExact(lm->etag)`. The etag is the one the job has just read, so the job
   deletes whatever meta is current: no state check and no round check.
3. `Pool/CasPool.cpp:197-199`: the fence on `gc_requests` is generation 0 forever. `admit` refuses only after
   `teardownBegun()`. The job passes no `Liveness` (`CasGcMetaWriter.cpp:164`). No lease check, fence token or
   `gc/state` check exists anywhere on the job's path.
4. `Gc/CasGc.cpp:1104` (`meta_pool_wait` drain) comes before `:1149` (the `gc/state` CAS). So the job runs
   whether or not its round later loses the CAS. The `SCOPE_EXIT` drain at `:612` only waits. It cancels
   nothing.
5. `Gc/CasGcScheduler.cpp:467-512` and `Gc/CasGc.cpp:5014-5033`: a follower steals only when both the lease
   tuple and the `gc/hb` pair are frozen across two of its ticks. The heartbeat thread keeps pulsing while the
   round thread is parked in `drain`. So a slow job alone cannot depose L1. The stall must also silence the
   heartbeat: a process freeze (SIGSTOP, VM pause, swap storm) or an S3 partition of L1.
6. `Gc/CasGc.cpp:2166-2168` (identical on the base): the graduation gate returns true on the in-process
   `(ref, token)` record without reading the marker. `Gc/CasBlobInDegree.cpp:493` (A0) skips the gate
   entirely for a carried `marker_confirmed` row. The redelete itself (`performRedeleteIo`, `CasGc.cpp:420-432`)
   never reads the marker.
7. `Pool/CasPartWriteTxn.cpp:424-455`: a writer that finds the blob present and the meta absent backfills a
   `Clean` meta and adopts the existing token (`Observed`). The only thing that stops adoption is a
   `Condemned` meta.
8. `Formats/CasBlobMetaFormat.h:39`: `BlobMeta::condemn_round` is documented as the field that tells a stale
   condemnation from a later re-condemn. Nothing uses it for that on the delete path.

## 1. Step 1: scheduling, waiting, fences

- **Order.** Body delete: `performRedeleteIo` (HEAD, then conditional `remove` by the observed etag). The
  outcome is applied on the round thread, and then `scheduleConfirmedMetaDelete` enqueues the job
  (`CasGc.cpp:466-468`).
- **Where it runs.** `GcMetaWriter::pool`: `gc_meta_pool_size` threads, default 16
  (`ContentAddressedSettings.cpp:81`). A round with more than 16 meta jobs leaves the rest queued.
- **What waits for it.**
  - `meta_pool_wait` → `drain()` → `pool.wait()` (`CasGc.cpp:1100-1108`). There is no timeout.
  - `drainOnExitNoThrow` on every exit (`:612`).
  - Both only wait. Neither cancels anything or checks leadership.
- **How long the stall must be.** Once the job is queued, a frozen L1 process leaves it queued. After a
  resume, the job admits a fresh operation, so it gets a fresh `Retry::standard()` window (90 s,
  `Backend/CasRetry.h:41`) and runs against the store as it is now.
  - L2's steal needs about two follower ticks plus L2's round. The default `gc_interval_sec` is 60.
  - The freeze must therefore last at least ~2-3 minutes. It must end before T1's redelete round (see §3).
  - A job already inside its GET when the freeze starts is harmless. Either its deadline expires, or it reads
    T0's marker and then its conditional delete loses. The harmful case is a job still waiting in the queue.
- **Partition variant.** A partition also freezes the heartbeat. Each queued job burns at most its 90 s
  window and then fails, and the pool wrapper swallows the error (`:116-128`). A queue deeper than
  (partition length / 90 s) × 16 still has jobs after the partition heals, and they run against the new
  state.
- **Fences.**
  - Lease check: none.
  - `CasOperation`: teardown only.
  - Fence token: generation 0.
  - Round CAS: after the drain, so it cannot stop the job.
  - Mount-lease fence-out of L1 by L2's `heartbeat_floor`: it acts on the mount plane only. The GC plane never
    sees it.
- **Result:** nothing stops the job.

## 2. Steps 2-3: T1, and whether T1's marker is the object L1 deletes

- **L2 inherits T0 as `delete_pending`.** L1's CAS never landed, so L2 folds from the same adopted state.
- **Path A: redelete.** L2 finds T0 `Gone` and runs its own meta-delete job, which removes T0's marker before
  L2's CAS. A writer then finds T0 absent and publishes T1. `reconcileMetaClean` creates a `Clean` meta
  (`CasPartWriteTxn.cpp:338-387`). T1 is dropped. `writeCondemnedMeta` changes `Clean` to `Condemned(c1)`
  through `casMeta` (`CasGcMetaWriter.cpp:77-78`). That is a new object version with a new etag.
- **Path B: supersede.** A shorter path exists. The writer creates and drops T1 before L2's first round. That
  round's `closeBlob` sees `peek_head` return T1 ≠ T0 and supersedes the entry (`CasBlobInDegree.cpp:547-563`).
  It condemns T1 at L2's first round and schedules the marker write (`CasGc.cpp:1027`). The minimum freeze is
  therefore the steal time plus one L2 round.
- **Is it the same object?** T1's marker is a different version from T0's marker, but it lives at the same
  key. That makes no difference to the current code: the stale job reads whatever is current and deletes it
  by the etag it has just read. In the reviewer's schedule, T1's marker is not "left alone".
- **The "left alone" case.** It happens only if T0's `Condemned(c0)` meta outlives T0's body and is still
  there when T1 is condemned (`CasGcMetaWriter.cpp:79`). That needs a double fault: L2's own meta delete is
  swallowed, and T1's publisher throws in `reconcileMetaClean` after its edge delta landed. Then T1's marker
  is literally T0's object. This matters for the fix (§6).

## 3. Step 5: what stops adoption or the delete once the marker is gone

- **Adoption gate.** In `ensureBlobPresent`, `loadMeta` refuses adoption only on `Condemned`. When the meta is
  absent, the writer backfills `Clean` and adopts T1 (`CasPartWriteTxn.cpp:424-455`).
- **Delete-time guard: none.** The redelete of T1 in round C+2 is HEAD plus a conditional `remove` by T1's
  etag. There is no marker read and no delta read after the cut.
- **What the three arms (`CasBlobInDegree.cpp:427-442`) cover:**
  - Arm 3 (in-degree recheck) sees only edges folded by C+2's cut.
  - Arm 2 (exact-token delete) matches, because the writer adopted T1 itself.
  - Arm 1 (floor) is already past.
- **Hazard window per writer.** The writer's edge (EDGE-BEFORE-OBSERVE) lands after C+2's cut and before
  C+2's delete. That is the fold-to-`pending_deletes` span of one round.
- **Writers in the gap.** A writer that lands between the marker loss and C+2's cut is spared by arm 3 and
  the entry drops. A writer in the C+2 window is not.
- **Hot hashes.** Common content (empty or constant column files) is re-inserted all the time, so the same
  hash recurs within minutes.
- **End state.** The adopter's committed manifest names a deleted blob: a dangling reference. This is loss,
  not a leak or a refused graduation.
- **Stale delete after graduation.** The stale delete can also land after graduation (C+1) and before the
  redelete (C+2). The marker is not read on either side of that point. Even a fresh re-read at graduation
  would not help.

## 4. The base

- **Same code.** The base has the same in-process confirmation: `condemnMarkerConfirmedInProcess` at base
  `CasGc.cpp:2087`, which is `:2168` here. `git diff` of the base against HEAD shows no change to the gate,
  the meta writer, the meta-delete site or the drain order.
- **Base schedule, no restart.**
  - L2 condemns T1 in round C, and its marker job records `(ref, T1)` in process.
  - L1's stale job deletes the marker.
  - Round C+1: `settleEntry` calls `confirm_condemned_marker`, which returns true from the in-process set.
    The entry graduates without reading the marker. The budget is not involved.
  - Round C+2 deletes T1 under the writer who adopted it in the window.
- **What the base does re-read.** A fresh process re-reads through `loadMeta` at graduation, and only for an
  unconfirmed, not-yet-pending row. So on the base a restart between the marker loss and graduation refuses
  the graduation and rewrites the marker (`:2192`). That is the one ordering A0 opens: a carried
  `marker_confirmed` row after a restart or leader change.
- **What the base never re-reads.** A `delete_pending` row, in any process.
- **Conclusion.** The hazard exists on the base. A0 widens it by one narrow variant: budget-exhausted carry
  plus restart. On the base, the common path (no restart, budget available) is already exposed.

## 5. Verdict details

- **Branch: REAL.**
- **Base: REAL, and it existed before this branch.** A0 is not the cause.
- **Stall needed.** An L1 freeze or partition that silences its heartbeat, with at least one meta-delete job
  still queued, lasting from the steal (about 2 ticks plus one L2 round, ~2-3 min at the default 60 s)
  until before T1's redelete round.
- **Other preconditions.**
  - The same content hash is re-created and dropped inside that window.
  - A writer adopts the hash inside C+2's cut-to-delete span.
- **Likelihood.** Rare per event (a minutes-long freeze of the GC leader), but not exotic: this is exactly the
  "safe at any leader staleness" model the pre-CAS delete comment claims (`CasGc.cpp:883-885`). Hot content
  hashes make re-creation likely rather than unlikely.

## 6. Fix

**Rule:** a meta delete names the condemnation it means. It removes only a `Condemned` meta whose
`condemn_round` is no newer than the retired entry's `condemn_round`. Anything else is left alone.

**Code shape:**

```cpp
void GcMetaWriter::scheduleConfirmedMetaDelete(const BlobRef & ref, uint64_t condemn_round);  // from entry.condemn_round

void deleteConfirmedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref, uint64_t condemn_round)
{
    const auto lm = loadMeta(op, layout, ref);
    /// A later generation's marker guards a live token; only this entry's condemnation may go.
    if (!lm || lm->meta.state != MetaState::Condemned || lm->meta.condemn_round > condemn_round)
        return;
    deleteMetaExact(op, layout, ref, lm->etag);
}
```

**Plus, in `writeCondemnedMeta`:** re-stamp an already-`Condemned` meta whose `condemn_round` is older than
the one being written: `casMeta` to the new round. Leave it alone only when it is equal or newer.

**Why it holds.** Round numbers only grow across leaders, because each round is the adopted `gc/state.round`
+ 1 and rebuild takes max + 1. A generation created after T0's body delete is condemned at a round greater
than c0.

- The job's read, compare and conditional delete act on one etag. A fresh marker for T1 therefore has a
  round greater than c0, so the job does nothing.
- The re-stamp gives a left-alone T0 object the round c1 and a new etag. A stale job then either sees c1 and
  does nothing, or holds the old etag and its conditional delete loses. That closes the double-fault case
  from §2.

**Costs.**

- No extra request on the normal path.
- A leftover `Condemned` meta over an absent body (the no-op branch) costs one small object. The next writer
  fixes it: it refuses to adopt, republishes, and reconciles the meta to `Clean`.
- A `Clean` meta is no longer deleted. It belongs to the live incarnation, and absence reads the same.
- The re-stamp costs one PUT only when a stale-round `Condemned` meta exists.
- Nothing downstream orders on the meta etag.

**No format change.** Both fields already exist, and the job captures the round in memory.

**Candidates rejected:**

- **(b) Leadership check in the job.** It adds one `gc/state` GET per meta delete, which this branch exists to
  cut. It is only sound as read-meta → check-lease → delete, and even then the left-alone double-fault case
  gets through, because the stale etag is still current.
- **(c) Drop A0.** It closes only "stale delete, then restart, then graduation". It does not close the base
  no-restart path. It also does not close the stale delete that lands between graduation and redelete, where
  the marker is never read. With fix (a) in place, A0 is safe as written.

## 7. TLA+ and gtest

**TLA+ (`CaGcCondemnMarkerGate.tla`).**

- Make `meta` a record `[st : {"absent","clean","condemned"}, cr : Nat]`.
- Add a variable `mdq`: the condemn rounds captured by outstanding meta-delete jobs. It is not fenced by
  `EndRound`, which models a deposed leader's job.
- Split `GSettleRedelete`:
  - Its body-delete branch sets `mdq' = mdq \cup {ret.cr}` and leaves `meta` unchanged.
  - A new action `GMetaDeleteJob(c)` for `c \in mdq`, enabled in any state:
    - Current code: `meta' = [st |-> "absent", cr |-> 0]`.
    - Fix: delete only if `meta.st = "condemned" /\ meta.cr <= c`.
- `GSettleCondemn`, `GSettleSupersede` and `GSettleCarryRetry` write `cr |-> round`. For the fix, apply the
  re-stamp rule.
- Run with `MaxGen >= 2` (T0 then T1) and `TwoWriters`.
- **Prediction:** `NoDangle` is violated on the current actions with `PersistConf = FALSE` and no `GRestart`
  (the base). It holds with the fixed `GMetaDeleteJob` plus the re-stamp.

**gtest.**

- **Unit test, failing first.** Build a `GcMetaWriter` on an in-memory pool. Seed blob T1 and meta
  `Condemned(c1)`. Call `scheduleConfirmedMetaDelete(ref)`, which stands for T0's job (c0 < c1), then
  `drain()`. Assert that the meta is still `Condemned(c1)`. It fails today, because the meta gets deleted.
  Add a sibling case for the left-alone object re-stamped by `writeCondemnedMeta`.
- **End-to-end test.** Two `Gc` instances (A, B) share one backend.
  1. An interposing backend parks the first `blobMetaKey(ref)` GET that A's meta pool issues after A's
     redelete of T0.
  2. A's round runs on a thread and blocks in `meta_pool_wait`. The parked request freezes its heartbeat,
     or the `gc/hb` writes are stubbed.
  3. B steals with an injected `now_fn`. A writer publishes T1 and drops it. B condemns T1 (supersede or
     fresh) and confirms it.
  4. Release A's GET.
  5. B runs round C+1 (graduate). In round C+2, `gc_redelete_apply_hook_for_test` runs a writer adopt whose
     edge lands after the cut.
  6. Assert `NoDangle`: the committed part's blob is present.

## Limits

- Static reading. Nothing was built, run or model-checked. The TLA+ prediction and both tests are
  predictions, not results.
- I did not measure the exact steal latency. "~2 ticks + one round" comes from the steal condition's
  structure (`CasGc.cpp:5014-5033`, `CasGcScheduler.cpp:400-402`), not from timing.
- I did not audit other callers of `writeCondemnedMeta`'s left-alone branch, or fsck's view of a leftover
  `Condemned` meta over an absent body, for side effects of the re-stamp.
- The claim that round numbers only grow rests on `new_round = state.round + 1` (`CasGc.cpp:645`) and on
  rebuild's `max(...) + 1` (`:4762`). I did not check other writers of `gc/state.round`.
