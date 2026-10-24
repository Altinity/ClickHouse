# Manifest-body memo vs a deposed leader: adjudication

Code: `/home/mfilimonov/workspace/ClickHouse/gc-request-cuts` @ `dfba328a747`. Paths are relative to
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`. This was a read-only review: nothing was built or run.

## Verdict {#verdict}

**SAFE, BUT IT WIDENS A KNOWN BASE HAZARD.**

- **Blob tokens: safe.** The memo cannot cause an exact-token blob delete that the delete gate is not entitled to make.
  The invariant is **veto completeness**: every edge that landed before the round's cut is folded, with true body content.
  - The absent-body clamp exists to withhold deletes when this invariant fails. It does not detect deposition.
  - A memo hit satisfies the invariant. It does not bypass it: the key is write-once, the body was validated when read, and the sign comes from the log.
- **`.meta` deletes: widened.** In the overlap schedule, a deposed L1 no longer suppresses its round. So it runs its whole
  `pending_deletes` batch, and every `Removed`/`Gone` outcome schedules a `deleteConfirmedMeta` job.
  - These jobs are the known defect's jobs, with the same per-job window.
  - The memo adds rounds that reach them. It does not add a new mechanism.
  - The same jobs are already reachable in the base: a deposed L1 that stalls *after* its fold is never suppressed.

Neither prior reviewer was exactly right:
- "Differs only in the safe direction" is wrong. The memo turns a suppressed round into an unsuppressed one, which is the less safe direction.
- "Changes the destructive gate" is true. But the gate's premise, a complete fold up to the cut, still holds on the memo path, so no blob delete loses its licence.

## 1. What a clamp does to L1's round {#clamp}

**The clamp itself** (`Gc/CasGc.cpp:2947-2978`):
- It is an anomaly (`:2952`) plus a `ManifestBodyMissing` hold at `log_id` (`:2972`).
- The staged `log_deltas` / `log_mf_cleanup` of that whole log are discarded (`:2977-2978`).
- The walk of **that namespace** stops (`break`). Its cursor stays at `resolved_through < log_id` (`:2974`, `:3034`).
- **Deltas never folded this round:**
  - every `+1` and `-1` of the clamped log, including edges before the clamp point in the same log;
  - every later log of that namespace, up to its snapshotted `committed_through`.
- Other namespaces fold normally.

**Consequence: global suppression.** `suppress_destructive = !anomalies.empty() || !carried_holds.empty() || frontier_incomplete`
(`:3445-3446`). One clamp suppresses the whole pool's destructive work. The reason is stated at `:3389-3393`: behind a clamped
cursor, "landed-before-cut events may be UNFOLDED", so the lemma "landed before cut ⇒ folded before graduation" fails.

Each step, and whether it depends on the fold (round order `:596-1409`):

| Step | Before or after the `gc/state` CAS | Depends on the fold? | Under a clamp |
|---|---|---|---|
| `pre_fold_ref_drain` (`:667-676`) | before | no; authority-checked | runs |
| heartbeat fence-out PUTs (`:693-736`) | before | no | runs |
| fresh condemns + `Condemned` marker writes (`CasBlobInDegree.cpp:565-576`, `CasGc.cpp:1027`) | before | yes: more folded history ⇒ more condemns | still run (not destructive) |
| graduation → `delete_pending` (`CasBlobInDegree.cpp:485-513`) | before; lives only in L1's seal | yes | suppressed (`:485`) |
| `pending_deletes`: exact-token blob deletes (`CasBlobInDegree.cpp:471-483`, `CasGc.cpp:924-934`, `:413-425`) | **before** | the fold can only **veto** (`indeg > 0` ⇒ spare, `:446-470`) | suppressed (carried unchanged `:476-477`; guard `:924-925`) |
| `deleteConfirmedMeta` jobs after `Removed`/`Gone` (`:466-469`) | **before** (drained at `:1104`) | via `pending_deletes` | none scheduled |
| `pruneSupersededGenerations` (`:1146`) | **before** | the gate only | suppressed |
| `gc/state` CAS (`:1151-1164`) | — | — | a deposed L1 throws `ABORTED` here |
| hand-off reclaim, manifest-body deletes (`mf_cleanup`), janitor, ref-object cleanup, orphan sweep (`:1195-1406`) | after | yes | never reached by a deposed L1 |

**A deposed L1 runs only the pre-CAS steps.** Of the fold-dependent destructive ones, that leaves three:
- `pending_deletes`,
- its `.meta` delete jobs,
- the retention prune.

There is no leadership fence in front of them:
- the GC plane's `CasRequests` fence is open (`Backend/CasRequests.h:150-152`);
- `refreshAuthority` (`Gc/CasGc.cpp:4933`) is called only for the janitor (`:378`) and the drain (`:5096`), never before `pending_deletes`.

## 2. What the extra folded history can do {#extra-history}

**What the memo changes.** When a clamp would have fired, L1 instead:
- folds A's edge from the memo,
- keeps walking that namespace up to the same snapshotted `committed_through`,
- ends the round unsuppressed (if nothing else is anomalous).

**The cut does not move.** `committed_through` is fixed before the walk (`:2789-2805`).

**Can an edge folded in round R make an entry deletable in round R? No.** Discriminator:
- `rmr.redelete` receives only `e` with `e.delete_pending`, where `e` is the **carried prior-run row** (`toRetiredEntry(cur_blob, *cur_condemned)`, `CasBlobInDegree.cpp:529`, `:471-483`).
- An entry graduated this round goes to `graduated`/`still_retired` (`:504-508`), never to `redelete`.
- A fresh condemn goes to `still_retired` (`:575`).
- So the pipeline is strictly condemn at C → graduate at C+1 → delete at C+2.
- L1's own graduations die with its failed CAS.

**Can an extra `-1` turn a veto into an allow? Only by cancelling a `+1` folded in the same round.**
- A `delete_pending` blob has no surviving prior edges (`CasBlobInDegree.cpp:448-451`, `:579-585`).
- An unmatched `-1` is a no-op (`:649-655`).
- So a `-1` lowers a `delete_pending` blob's in-degree only by cancelling a `+1` on the same `(blob, source_id)` from earlier in this fold.
- That pair is A's own publish and drop, both at or below the cut. The true state at the cut is "A unbound", so the fold's in-degree is correct.

**The extra `+1` edges only add vetoes.** The extra `-1` edges only add condemns, which are not destructive.

**Can L1 delete a token that the clamped round would not? Yes.** The clamped round deletes nothing, so every entry in the memo round's `redelete` is "extra".

**Is each such token garbage?** Yes, by the base argument, which the memo leaves intact:
- The entry was `delete_pending` in the committed parent seal P.
- Graduation required durable `Condemned` evidence for exactly that `(hash, token)` (`CasBlobInDegree.cpp:485-493`, `CasGc.cpp:2157-2176`).
- A same-token adopter saw a non-`Condemned` meta earlier. Its edge landed before the marker was durable (EDGE-BEFORE-OBSERVE, `:2159-2164`), so before P committed, so before L1's round started. So it is below L1's cut.
- L1's fold of that namespace is complete up to the cut, because the memo hit replaced the only gap. So the veto is present.
- Any later reference uses a fresh envelope (`ensureBlobPresent` refuses to adopt over `Condemned`, `CasGc.cpp:967-977`). L1's HEAD-then-conditional delete (`:421-423`) then returns `Mismatch`.

## 3. The deposed-leader base case {#base}

**Without any memo, a deposed L1 already runs `pending_deletes` whenever its fold saw no absence.** Examples:
- it stalls after `fold()` returns;
- the successor deleted nothing that L1 still had to read.

**What protects tokens in the base** does not depend on staleness:
1. **`delete_pending` in a committed parent.** This is previously published state; the comment at `:882-884` says "safe at any leader staleness".
2. **The durable `Condemned` marker before graduation.** Writers cannot adopt the token after the marker is durable.
3. **EDGE-BEFORE-OBSERVE.** It puts every pre-marker adopter's edge before P's commit, so inside any later cut.
4. **The exact-token delete.** A token deleted by L2 and re-created by a writer is a new token, so L1 gets `Mismatch` and deletes nothing.
5. **The in-degree re-check against a complete fold.** The clamp gate exists for the case "complete fold" is false (`:3389-3393`).

**The memo leaves all five intact:**
- It does not change the parent, the markers, or the cut.
- It does not change the token comparison.
- It keeps the fold complete. It does not declare an incomplete fold complete: the memo stores only validated bodies and never stores absence (`Gc/CasGcManifestMemo.h:29-31`, `Gc/CasGc.cpp:1539-1540`).

**So the absent-body clamp has no safety role in the overlap.**
- In this schedule, the body is gone only because a successor committed. That makes L1 stale.
- Staleness is covered by 1–4, not by the clamp.

**L2 already acted before L1 resumes.** Cases:
- L2 deleted the same token: L1's HEAD finds it `Gone`.
- A writer re-created the blob: new token, so `Mismatch`.
- L2 spared the entry because of a `+1` beyond L1's cut: that `+1` is either a fresh envelope (`Mismatch`) or impossible as a same-token adopt, by (3).

## 4. Interaction with the delayed `.meta` delete defect {#meta-hazard}

**Yes, the memo widens it, in schedule space only.**

**Which jobs:** one `scheduleConfirmedMetaDelete` per L1 `redelete` entry whose outcome is `Removed` or `Gone` (`Gc/CasGc.cpp:466-469`).
- There are at most `gc_round_redelete_budget` of them per round (`CasBlobInDegree.cpp:476-483`).
- In the overlap, L2 usually already deleted the same parent entries, so almost every outcome is `Gone` and schedules a job.

**Under what condition the memo reaches them** (all four must hold):
- (a) L1 memoized manifest A at an earlier edge of this fold, and A was not evicted.
- (b) A successor committed a round that folded A's removal, and ran its post-CAS manifest deletes, before L1 reached A's later edge.
- (c) The successor did not trim any ref log L1 still had to read. A trimmed log gives a `GapBelowWitness` hold, which suppresses the round anyway.
- (d) No other anomaly, hold, or unproven frontier.

Without the memo, (a)+(b) produce a clamp, and L1 schedules **zero** such jobs.

**Per-job window: unchanged.** It runs from L1's HEAD (`Gone`) to the job's `loadMeta`+`deleteMetaExact` (`Gc/CasGcMetaWriter.cpp:86-92`), bounded by the `meta_pool_wait` drain (`Gc/CasGc.cpp:1104`).

**No new jobs or windows in the base's sense.** A deposed L1 that stalls after its fold reaches the identical job set.

**Also widened, but not destructive:** more fresh condemns, and so more `Condemned` marker writes, by a deposed L1. Markers are conservative: writers re-upload instead of adopting.

## 5. Verdict {#verdict-detail}

**SAFE BUT WIDENS A KNOWN BASE HAZARD.**

- **Invariant:** veto completeness up to the cut. Every edge landed before the round's cut is folded with its true body, and the delete gate needs nothing more.
- **Widening:** the `.meta` delete defect of section 4, quantified there. Up to `gc_round_redelete_budget` extra jobs, in rounds that the base would suppress when L1's stall falls inside its fold, before a memoized body's later edge.

## 6. Minimal fix {#fix}

**Nothing is needed for blob safety.** If the lead wants base behaviour back in the overlap:

**Recommended:** call `refreshAuthority(state.lease.seq)` once, after `fold()` returns and before `pending_deletes` (`Gc/CasGc.cpp:859-890`). If `!authority_held`, throw `ABORTED`.
- **Cost:** 1 GET of `gc/state` per folding round.
- **Why it closes the overlap:**
  - a successor can delete a folded body only after it committed a round, which comes after its steal;
  - a steal changes `lease.owner`/`seq` for good;
  - so a check that passes after the fold proves no memo hit differed from a fresh read.
- **Side benefit:** it also narrows the base after-fold stall window.
- **What it leaves open:** the check-then-act gap. A steal after the check is the base exposure, closed only by fixing the `.meta` defect itself.

**Rejected alternatives:**
- **HEAD revalidation of removal hits:** one HEAD per removal hit. On S3 and GCS a HEAD is billed in the same request class as a GET, so this gives back most of the memo's request savings for publish-then-drop pairs.
- **Dropping the memo when the lease sample changes:** it needs a per-edge `gc/state` read to be sound, which costs far more than one GET.

## 7. Deterministic gtest {#test}

Put it in `src/Disks/tests/gtest_cas_gc_fold.cpp`, using `MemoFixture` (`:793`) with `io_concurrency = 1`.

**Setup:**
1. Run L1 = `f.gc` for rounds that leave blob X `delete_pending` in the parent seal:
   - publish P→X, then drop P;
   - round 1 condemns X (token `t`), round 2 graduates it.
2. Keep blob Y referenced by a live manifest Q.
3. Create L2 = `Gc(f.store, hexToU128("…0c"))` with the same memo setting.

**Stage round 3:** call `publishThenDrop(f, a)` (`:911`), which returns the drop log key. Register `f.backend->onRead(drop_log, hook)`, the idiom at `:1160`. The hook:
1. Runs `L2.runRegularRound()` until it acquires the lease: steal after two identical observations (`gtest_cas_gc_round.cpp:245-259`), at most 3 calls. Assert that L2 committed.
2. Asserts L2 deleted A's body and X@`t`.
3. Has a writer re-upload X (fresh token `t'`) and publish manifest R→X.

**Run L1:** `EXPECT_THROW(f.gc->runRegularRound({}, true, policy, &progress), DB::Exception)`. Inspect `progress`.

**Assertions:**

| Assertion | memo = on | memo = off (oracle) | Holds today? |
|---|---|---|---|
| A clamp fires | no anomaly for `kMemoNs`, 1 memo hit | anomaly for `kMemoNs` | holds by analysis |
| `progress.redeleted` | ≥ 1 (X, outcome `Replaced`) | 0 | holds; this is the widening |
| Stale commit rejected: `gc/state` lease owner is L2, round = L2's | yes | yes | holds (`:1151-1163`) |
| X at `t'` still exists | yes | yes | holds (exact-token `:421-423`) |
| Y still exists, in-degree 1 | yes | yes | holds (`indeg > 0` veto) |
| With the fix: L1 throws at the authority check, `redeleted == 0` | — | — | red today |

**Optional second case**, for the `.meta` defect:
- Use `gc_redelete_apply_hook_for_test` (`:429`), gated by a flag so it fires only for L1. Inside it: re-upload X, then commit a successor round that writes a new `Condemned` for X.
- Assert the marker survives L1's drained job.
- This is red today in both the memo overlap and the after-fold stall. It pins the base defect, not the memo.

## 8. Replacement comments {#comments}

**`Gc/CasGc.cpp:1498-1499`:**
```
/// A memoized body stands in for a fresh read: the key is write-once, so a hit is the body's true
/// content. It differs from a fresh read only after a successor leader deleted the body, and then the
/// fold is still complete up to its cut, which is all the delete gate needs. The sign and the ordinal
/// always come from this edge, never from the memo.
```

**`Gc/CasGcManifestMemo.h:29-31`:**
```
/// Validated manifest bodies of ONE fold, keyed by `ManifestId`. A hit is the body's true content
/// (the key is write-once); it differs from a fresh read only when a successor leader deleted the body
/// during the fold, and that fold's round can no longer commit. An instance must not outlive its fold.
/// Absence is never stored: only `insert` of a validated body adds an entry.
```

**If the authority check is adopted**, replace the second sentence of the header with:

> ...only when a successor leader deleted the body during the fold, and such a round fails its authority check before any delete.

**`src/Disks/tests/gtest_cas_gc_fold.cpp:1102`** repeats the old premise ("a fresh read only while no folded body is deleted during the fold"). It needs the same correction: that test pins that **no non-successor actor** deletes a folded body.

## Limits {#limits}

**Not re-verified here:**
- **EDGE-BEFORE-OBSERVE** (writer edge before meta observation). I took it from the comment at `Gc/CasGc.cpp:2159-2164`, not from `PartWriteTxn`. The blob-safety verdict depends on it. But it depends on it equally with or without the memo.
- **The list of manifest-body deleters.** I checked these: GC post-CAS `mf_cleanup`, the orphan sweep, the janitor, writer debris cleanup (`Pool/CasPartWriteTxn.cpp:1228`), and fsck. Only the first three delete an owned body, and all three run post-CAS under a committed lease.
  - Test `FoldedManifestBodyIsNeverDeletedDuringTheFold` pins that the sweep and writer abandon do not delete a folded body.
  - I did not verify multi-owner removal: a `-1` for one owner while another still binds A queues A for deletion (`:1537-1538`). That is outside the memo question.
- **The retention prune by a deposed L1.** Its safety is the base's accepted slack (`:1140-1144`). The memo only lets it run in more schedules.
