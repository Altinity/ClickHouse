---
description: 'Design for fixing two related writer-side content-addressed bugs: PROMOTE-OVER-COMMITTED-LEAK (Build::promote silently overwrites a committed ref, orphaning the old manifest) and ABANDON-RETIRE-ORDERING (Build::abandon retires the build_seq before emitting the precommit removal). Fail-close promote + idempotent republishRef re-drive + retire-after-removal.'
sidebar_label: 'Promote-over-committed leak fix'
sidebar_position: 52
slug: /superpowers/specs/cas-promote-over-committed-leak-fix
title: 'CAS — promote-over-committed leak + abandon-retire-ordering fix design'
doc_type: 'guide'
---

# CAS — promote-over-committed leak + abandon-retire-ordering fix design {#promote-over-committed-leak-fix}

Two related writer-side bugs in the content-addressed build/commit path, fixed together (same files:
`CasBuild.cpp` `Build::promote`/`Build::abandon`, `ContentAddressedTransaction.cpp` `republishRef`).
Backlogged as `PROMOTE-OVER-COMMITTED-LEAK` and `ABANDON-RETIRE-ORDERING` in
`utils/ca-soak/scenarios/BACKLOG.md`.

## Problem {#problem}

### BUG 1 — `PROMOTE-OVER-COMMITTED-LEAK` {#bug1}

`Build::promote` (`CasBuild.cpp` ~L896–907) unconditionally overwrites `root.refs[final_ref_name]` and
appends only a Δ=0 owner-move `RootOwnerEvent` (`old = Precommit(R, bld, T)`, `new = Committed(R, T)`). It
never checks whether `refs[R]` already names a **different** committed manifest `T_old`, and never emits
the `-1` (repoint) that would release `T_old`. So promoting over a pre-existing committed ref leaves
`Committed(R, T_old)` live in the journal with no `-1` → `T_old`'s manifest body and its uniquely-owned
blobs are pinned forever (in-degree stuck ≥ 1); the journal holds two live `Committed` bindings for one
ref; a later `dropRef(R)` removes only `Committed(R, T_new)`, leaving `Committed(R, T_old)` live with no
`refs[R]` entry (owner↔refs divergence). This is a **liveness/space leak + owner↔refs divergence**, not a
dangle/data-loss (`INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_COMMIT_FAILCLOSED` hold). It is a **fail-close-principle
violation**: `promote` silently assumes the unique-ref invariant instead of enforcing it.

**Reachability (real, not just "if unique-ref is externally violated").** `republishRef`
(`ContentAddressedTransaction.cpp:143`, the primitive behind `RENAME TABLE` and `DETACH`/`ATTACH` renames
via `moveDirectory`) does `stageManifest → precommitAdd → promote(dst) → dropRef(src)`. Its only
idempotency gate is `if (!resolveRef(src)) return false` — it no-ops solely when the **source** is gone. A
crash/throw after `promote(dst)` and before `dropRef(src)`, then re-driven (the `moveDirectory` loop
advertises re-drivability), finds src still present → NOT a no-op → re-stages a **fresh** `ManifestId T_b`
(`stageManifest` bumps the ordinal; a new build ⇒ new `build_sequence`) and `promote(dst)` overwrites
`refs[dst] = Committed(T_b)`, leaking `Committed(dst, T_a)` from the first attempt. So the leak is reachable
by any partially-completed `republishRef` re-drive.

### BUG 2 — `ABANDON-RETIRE-ORDERING` {#bug2}

`Build::abandon` (`CasBuild.cpp:929`) calls `store->retireBuildSeq(build_seq)` **before** appending the
precommit-removal `mutateShard` (~L942–956). `retireBuildSeq` is what lets the mount watermark `min_active`
advance past this `build_sequence`, which is precisely what makes GC's `reclaimAbandonedPrecommit` judge
the precommit **dead**. So there is a window where the precommit is dead-but-not-yet-removed, and GC can
append its own removal while `abandon` also appends one — a double removal of the same precommit binding.
Benign today (in-degree is an idempotent source-edge **set**, so a double `-1` is absorbed), but it
contradicts the ordering discipline the code documents, and `Build::promote` already does the safe order
(retire only **after** its CAS, `CasBuild.cpp:911`). The just-landed `DANGLING-PRECOMMIT` fix force-Reads
Skip-parked shards, **increasing** `reclaimAbandonedPrecommit`'s firing frequency, so this window is
exercised more often.

## Fix {#fix}

Chosen approach: **fail-close `promote` + idempotent `republishRef` re-drive** (not a silent repoint), and
**retire-after-removal** for `abandon`. No `LOGICAL_ERROR` anywhere — the refusal cases throw `ABORTED`
(the code ClickHouse uses for "operation refused due to conflicting durable state", and the exact code
`promote` already throws in its other fail-closed branches; `LOGICAL_ERROR` is reserved for
must-not-happen invariant violations and is CI-checked, so it is wrong for a reachable runtime refusal).

### BUG 1a — `Build::promote` fail-close guard {#fix-1a}

In the `promote` `mutateShard` closure, before overwriting `root.refs[final_ref_name]`, inspect the
existing entry:

- If `refs[final_ref_name]` exists and names a **different** committed `manifest_ref` than `id.ref` →
  `throw Exception(ErrorCodes::ABORTED, ...)` ("promote refuses to overwrite a live committed ref with a
  different manifest — unique-ref invariant; use `republishRef` for an intended repoint"). This restores
  the tripwire the code's own comment worried about; no silent overwrite, no leak.
- If the entry is absent, or names the **same** `manifest_ref` (an idempotent re-promote of the same
  content) → proceed as today.

This never performs a repoint (releasing `T_old` silently) — a committed MergeTree ref is never
legitimately overwritten with different content under the same name (parts are immutable; mutations mint
new names), so a different-content overwrite is always either a bug or a re-drive, both of which this
guard + BUG 1c handle without a silent release.

### BUG 1c — `republishRef` idempotent on the destination {#fix-1c}

After `resolveRef(src)` (unchanged: returns false when src is gone), also `resolveRef(dst)`. If dst is
already committed:

- if the dst manifest's `entries` equal `src_manifest.entries` (this rename's own prior `promote` already
  landed; only `dropRef(src)` was interrupted) → **skip** `stageManifest`/`precommitAdd`/`promote`, go
  straight to `dropRef(src)`, and return true (idempotent completion of the interrupted re-drive);
- else (dst committed to **different** content — a genuine `ATTACH`-onto-existing-name / rename conflict) →
  `throw Exception(ErrorCodes::ABORTED, ...)` (do not silently drop src, which would lose its content).

If dst is absent → the normal path (`stageManifest`/`precommitAdd`/`promote`/`dropRef`) runs unchanged.

Idempotency is keyed on **content** (`entries`), not `ManifestId`, because the re-drive mints a fresh id
for the same content. Compare **only** `PartManifest::entries` (the `std::vector<ManifestEntry>` with its
defaulted element-wise `operator==`), NOT the whole `PartManifest`: `encodePartManifest` writes entries in
canonical **path-sorted** order (`CasManifestCodec.cpp:67-76`, dup-path rejected) and `decodePartManifest`
reads them back in that order, so `readManifest(...).entries` is deterministically ordered and the vector
`==` is order-stable; the full-manifest `==` would be a false conflict because `ref`, `root_namespace_id`,
and `payload_digest` legitimately differ between src and dst (the digest is a content-hash over
ref+namespace+entries). This makes `RENAME TABLE` / `DETACH`-`ATTACH` re-drives idempotent, so the second
`promote` never runs → no `T_b` overwrite → no leak; and it never reaches `promote`'s fail-close guard
(it skips `promote` entirely when dst is already committed).

### BUG 2 — `Build::abandon` retire-after-removal {#fix-2}

Move `store->retireBuildSeq(build_seq)` from before the precommit-removal `mutateShard` to **after** it —
unconditionally, past the `if (precommitted) { … }` block (so it still runs for a build that never
precommitted), mirroring `Build::promote` (retire only after its CAS). Keep `alive = false` early. The
precommit then becomes watermark-dead only **after** its removal is durably committed, so
`reclaimAbandonedPrecommit` can never observe a live-and-dead precommit to double-remove. Pure reorder;
`retireBuildSeq` is idempotent, the removal stays reliable (`mutateShard`), and the best-effort debris
cleanup still runs afterwards.

## Safety {#safety}

- **1a:** fail-close — throws instead of overwriting, so it can never leak; the idempotent same-manifest
  re-promote is still allowed; aligns with the fail-close principle. `ABORTED` is caller-handleable, not a
  CI-flagged `LOGICAL_ERROR`.
- **1c:** idempotency is gated on (dst committed ∧ content match); a different-content conflict fails
  closed (no silent src data loss). No leak (no second `promote`). Preserves `INV_NO_LOSS` (src content is
  reachable at dst before src is dropped).
- **2:** pure reorder; no new failure mode; closes the double-removal window; double `-1` was already
  absorbed by the idempotent source-edge set, so this is defense-in-depth + ordering hygiene.
- All three preserve `INV_NO_DANGLE`/`INV_NO_LOSS`: no committed ref is left dangling and no content is
  lost or silently orphaned.

## TLA+ {#tla}

Gate in `CaGcRootLocalPartManifestCore` (already models `promote`, repoint, `republish`, and the
two-committed-owners hazard via `SabotageTwoOwners`). The task will either reuse `SabotageTwoOwners` (two
live committed bindings for one ref = exactly BUG 1's leaked `T_old`, which already violates `INV_NO_LOSS`)
or add a focused `SabotagePromoteOverwritesCommitted` control, showing the leak is reachable when
`promote` overwrites a committed binding without releasing it and unreachable with the fail-close guard.
Model the `republishRef` re-drive idempotency (dst-committed → skip → `dropRef(src)`) and confirm it
preserves `INV_NO_LOSS` with no leaked binding. Bug config reachable / fix config holds, checked before the
C++ change. Confirm no existing positive stage or sabotage counterexample is disturbed by the new guard.

## Testing {#testing}

TDD, in the CA build/transaction gtests (`src/Disks/tests/`):

1. **BUG 1a:** `promote` over a pre-existing **different** committed ref throws `ABORTED`; `promote` over
   the **same** `manifest_ref` (idempotent re-promote) succeeds; `promote` over an absent ref succeeds
   (the normal path).
2. **BUG 1c:** `republishRef` re-drive after a simulated crash-before-`dropRef(src)` (dst already committed
   with the same content) is idempotent — it skips the publish, drops src, mints no second manifest, and
   leaves no orphaned first-attempt manifest; a dst committed to **different** content throws `ABORTED`.
3. **End-to-end:** a `RENAME`/`DETACH`-`ATTACH` re-drive leaves no orphaned `T_a` manifest and no owner↔refs
   divergence (fsck clean; no pinned manifest/blobs).
4. **BUG 2:** `abandon` emits the precommit removal, and the reorder is exercised — a focused test that a
   GC `reclaimAbandonedPrecommit` running against an abandoned build does not double-append a removal
   (or, minimally, that the removal is committed before `retireBuildSeq` makes the precommit reclaimable).
5. **Scenario:** a rename/attach-churn ca-soak card shows no `owner↔refs` divergence and no
   promote-over-committed leak at the quiesced fixpoint.

## Out of scope {#out-of-scope}

- `INTROSPECTION-1`/`INTROSPECTION-2` (separate debuggability cycle).
- A silent repoint feature for `promote` (deliberately rejected — it would silently accept an overwrite,
  against the fail-close principle, and mask a unique-ref violation in the normal insert path).
- The `DANGLING-PRECOMMIT` and blob `RESURRECT-REUPLOAD-ORPHAN` fixes (already landed).

## Docs to update after the fix lands {#docs-to-update}

`docs/superpowers/cas/06-tla-models.md` (record the promote-over-committed gate + that the C++ fix landed);
`utils/ca-soak/scenarios/BACKLOG.md` (`PROMOTE-OVER-COMMITTED-LEAK` and `ABANDON-RETIRE-ORDERING` → resolved
once the tests + scenario are green).
