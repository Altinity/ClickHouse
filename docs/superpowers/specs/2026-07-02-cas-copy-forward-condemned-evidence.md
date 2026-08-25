# CAS: verified copy-forward for condemned evidence deps — spec + plan

**Status:** APPROVED 2026-07-02 (user: "Если TLA+ модель подтвердит что это безопасно — ок";
Task 0 is that gate). **Branch:** `cas-copy-forward` off `cas-gc-snapshot-streaming`.
**Scope constraint (user, verbatim):** no `ReplicatedMergeTree` changes — the fix lives entirely
in the CA layer.

## Problem

Soak run 3 (2026-07-02): a replica restart leaves a table readonly PERMANENTLY. Chain:
`ReplicatedMergeTreeAttachThread::runImpl` → `checkParts` → `renameToDetached` (part CA-committed
but ZK-unknown: kill landed between the CA commit and ZK `commitPart`) →
`ContentAddressedTransaction::moveDirectory` → `republishRef` → `Build::promote` →
`observeAndAdmit` sees the dedup blob's token condemned in the fresh retire-view → `ABORTED`
("condemned at commit revalidation, INV-1") → "Initialization failed, table will remain readonly".
The blob was legally adopted by stale-view writers on both replicas; both were killed before the
+1 edges folded; the restart opened a FRESH view where the entry is visible. Nothing retries the
attach; the abort is a liveness brick, not a safety save.

`republishRef` records its deps via `adoptEvidence` (tokenless W-EVIDENCE) and `promote` re-proves
each fail-closed. The "(no source): propagates ABORTED (retryable)" assumption in `observeAndAdmit`
is false for this caller.

## Rejected alternatives (why copy-forward)

- **Recreate from the writer's local files** — does not exist: a CA part's "files" ARE the pool
  blobs; there is no independent source on the move path.
- **Δ=0 same-`ManifestId` owner-move rename** — `shardOf(ref_name)` differs for the new name in
  (root_shards−1)/root_shards of moves; a cross-shard two-event decomposition creates two-owner /
  zero-owner windows that the source-edge set model cannot represent (one manifest = one
  `source_id` set). Single-owner `ManifestId` discipline stands; fresh-id republish stands.
- **Relaxing the gate for non-pending entries** — refuted by the existing model:
  `SabotageAdoptRetiredToken`'s counterexample is exactly the prepare→land window around a
  graduating pass. Binding a listed token is never safe.

## Fix: verified copy-forward

A dep recorded by `adoptEvidence` always originates from a COMMITTED manifest (all four call
sites: `republishRef`, the fetch receiver, part copy, per-file copy) — the blob is reachable
through a live committed owner right now; this is a reference transfer, not a resurrection of
garbage. For exactly these tokenless-evidence deps, `observeAndAdmit` on a condemned token stops
aborting and instead **copies the incarnation forward**:

1. `backend().get(key)` the full object (yes, a read of the dying object — see the invariant
   amendment below). Verify fail-closed: envelope decodes, `kind` matches, and the payload's
   recomputed logical hash equals `hash`. Any mismatch / 404 ⇒ `ABORTED` exactly as today.
2. Re-wrap the payload with a FRESH envelope header (fresh `incarnation_tag`, this build's
   `build_id` — same W-FRESH-TAG rule as `uploadFromSource`, B167).
3. `putOverwrite(key, bytes, expected = observed token)` ⇒ fresh token `t′`; admit `t′` as the dep.
   `PreconditionFailed` ⇒ re-`head`: absent ⇒ `ABORTED` (fail-closed — the delete won); token
   changed ⇒ re-evaluate from step 0 (someone else recreated: adopt if clean, copy-forward if the
   new token is also condemned). Bounded attempts (8) ⇒ `ABORTED`.
4. Emit a `CasEventType::BlobCopyForward` event + a profile counter; the old retired entry
   `(hash, t)` stays listed and settles at the next fold: its exact-token delete mismatches `t′`
   and the entry drops without touching the new incarnation (already the merge's rule).

Sourced paths (`putBlob`, tree recreate) keep re-upload-from-source unchanged. Callers other than
`adoptEvidence` (bodyless gate deps) keep the abort.

### Invariant amendment (recorded in `feedback_ca_resurrect_invariant` memory + docs)

"Never read/GET a condemned object to revive it" gains one narrow exception: **verified
copy-forward of a condemned incarnation that is still referenced by a live committed manifest** —
allowed only with (a) committed-source evidence (`adoptEvidence` deps), (b) full content
verification on read (envelope + recomputed logical hash == key hash), (c) token-conditional
`putOverwrite@observed-token`; every failure mode stays fail-closed (`ABORTED`), never a blind PUT,
never `putIfAbsent` after a lost delete race.

### TLA+ correspondence (Task 0 gate)

`CaGcAckFloorCore`'s `WPrepare` recreate branch already models "mint a fresh incarnation while the
entry is visible, bind only the fresh token" — the byte source is below the model's abstraction,
and the atomic mint is a faithful abstraction of the token-conditional `putOverwrite@t` (CAS on
token). `GComplete` already proves the aftermath (`kills == {e : tok[e.b] = e.t}`: a mismatched
graduated entry drops without a delete). The gate makes this machine-checked rather than argued:
a distinguished `WCopyForward(w, b)` action — enabled only when `present[b]` ∧ entry for `tok[b]`
visible in `wView[w]` (NO `~present` arm: copy-forward has no source and fail-closes there) — plus
a `W_CopyForwardHappens` witness. Required results: stage-1 clean run stays clean
(`INV_NO_DANGLE`, `INV_NO_RETURN`, `INV_ACK_LE_VIEW`), the witness fires, and every existing
sabotage cfg still produces its counterexample.

## Plan

### Task 0 — TLA+ gate
- `docs/superpowers/models/CaGcAckFloorCore.tla`: add `copyForwardEver` flag, `WCopyForward(w, b)`
  (guards: live, `wPending = {}`, `nextTok ≤ MaxTok`, `present[b]`, visible entry for the CURRENT
  token; transition: same fresh-mint + bind as the recreate arm), wire into `Next`, add
  `W_CopyForwardHappens == ~copyForwardEver`, extend the module comment with the
  putOverwrite@t-faithfulness argument.
- Add `CaGcAckFloorCore_witness_copyforward.cfg`; rerun `run_ackfloor.sh` (stage-1 clean + all
  sabotages + witnesses). Gate: clean stays clean, witness fires, sabotages still fire. Commit.

### Task 1 — failing-first repro (red)
- `src/Disks/tests/gtest_cas_build.cpp`, InMemory backend, mirrors `republishRef` line-for-line:
  publish part A over blob X → run a GC round (edges fold) → `dropRef` A → run rounds until X's
  entry is condemned in the retired list → with a STALE-view store handle, publish part B adopting
  X (legal, passes) → NO round → reopen the Store (fresh view, entry visible) → `startBuild` +
  `adoptEvidence(X's entry)` + `stageManifest` + `precommitAdd` + `promote` ⇒ today `ABORTED`.
  Assert the post-fix contract (promote succeeds; dep token ≠ old token) so the test is red now,
  green after Task 2. Commit as failing (`git commit` with `[expected-red]` note in message).

### Task 2 — implementation (green)
- `Core/CasBuild.{h,cpp}`: mark deps recorded by `adoptEvidence` (the existing tokened/tokenless
  distinction in `DepEntry` already separates them — reuse it); in `observeAndAdmit`'s condemned
  branch, tokenless-evidence deps take `copyForwardFromCondemned(kind, hash, key, hr)` (new,
  next to `uploadFromSource`; steps 1–3 above, attempt bound 8) instead of throwing; everything
  else unchanged.
- Race/unit tests alongside the repro: (a) token drifted before the overwrite and the new token is
  clean ⇒ adopt without a second copy; (b) object deleted mid-flight ⇒ `ABORTED`, nothing written;
  (c) corrupt payload (hash mismatch) ⇒ `ABORTED`, nothing written; (d) after copy-forward, the
  next rounds drop the old entry WITHOUT deleting the new incarnation (`runRoundsUntilAbsent` +
  fsck-style dangle check = 0).
- Repro from Task 1 green. Full `Cas*` gtest suite green. Commit.

### Task 3 — observability + docs + sweep
- `CasEventType::BlobCopyForward` (emitted in the primitive) + `ProfileEvents` counter
  (`CasBlobCopyForward`); extend the `gtest_cas_event_log.cpp` coverage for the new event.
- Docs: `docs/superpowers/cas/02-write-path.md` (or the invariants section that states INV-1) gets
  the amended invariant text; ROADMAP row for the follow-up "promote-time copy-forward for
  SOURCED deps that lost their source" stays queued.
- Update `feedback_ca_resurrect_invariant` memory with the exception, `project_ca_gc_ack_floor_fence`
  memory with the outcome. Full suite + full link if `Core` headers changed. Commit.

### Validation (queued, not in this plan)
Fresh soak run on a clean pool replays the S13-adjacent kill-restart chaos; the attach path must
recover (grep for `BlobCopyForward` events + zero "table will remain readonly" without recovery).

## Implementation notes (landed 2026-07-02)

Landed on `cas-copy-forward` (Tasks 0-3). Deviations and findings, all deliberate:

1. **Corrupt payload throws `CORRUPTED_DATA`, not `ABORTED`.** Data damage is not retryable;
   the spec's "any mismatch ⇒ ABORTED" was imprecise. 404/lost-race stays `ABORTED`.
2. **The RAW-displacement test found a non-fact, later corrected (2026-07-02 brainstorm):** the
   test's "abandoned copy-forward orphan" exists only in the raw shape (a `putOverwrite` with no
   owner events). In the REAL flow `republishRef` lands `stageManifest` + `precommitAdd` BEFORE
   the promote pre-pass (reachability-before-content, B188): a writer dying after the copy-forward
   PUT leaves a folded +1 (spares the old entry), then `reclaimAbandonedPrecommit`'s -1 transitions
   the blob to zero, a FRESH `(hash, t1)` entry condemns it, and the pipeline deletes it — full
   self-healing, no orphan. What the test pins (still valid): the stale `(hash, t0)` entry drops on
   exact-token mismatch and the fresh incarnation is never wrong-token-deleted.
3. **`blob_copy_forward` event + `CasBlobCopyForward` counter landed with Task 2** (the primitive
   emits them), not Task 3.
4. **Task 3 was extended with writer/mount introspection insights** (user request, same day):
   `retired_view_advance` event per view advance (installed round, `from_round`, `retired_entries` loaded),
   `mount_remount` event (ok/failed), and the GC round log gained the ack-floor pipeline columns
   (`entries_condemned/graduated/redeleted`, `fence_outs`, `min_ack`, `anomalies`), replacing the
   dead always-0 `children_cascaded`/`forgotten_*` columns (pre-release: no compat scaffolding).
5. **`FreshEvidenceDepWithViewHitIsResolvedByGate` updated**: "caught by the gate" now means
   resolved by displacement (promote succeeds; the listed token is never bound), not `ABORTED`.
