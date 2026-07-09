# CAS promote: close the tokenless copy-forward condemn-race (DETACH/freeze adopt path)

**Date:** 2026-07-09
**Branch:** `cas-gc-rebuild`
**Status:** design (root cause confirmed + mechanism validated by a fresh-model adversarial consult)
**Follow-up to:** `2026-07-09-cas-promote-resurrect-tokened-blob-design.md` (the tokened INSERT fix, landed)

## Problem

After the tokened-INSERT resurrect fix landed and validated (01156/01710/02346 green under load), the full
CA-s3 lane surfaced `03283_optimize_on_insert_level`:

```
promote: blob <h> condemned at commit revalidation — failing closed (INV-1). (ABORTED)
```

Query: `ALTER TABLE ... DETACH PARTITION tuple();` → `StorageMergeTree::dropPartition` →
`makeCloneInDetached` → freeze → CAS commit → `Build::promote` → the `!src` tokenless backstop
(`CasBuild.cpp:931`). This is the **tokenless** manifestation of the same promote condemn-race: the clone
adopts source blobs by hash via `adoptEvidence` (no `putBlob`, so no retained `BlobSource`), so the tokened
resurrect (`uploadFromSource`) does not apply and the leaf fails closed.

Not a regression: pre-fix promote aborted on **every** condemned leaf; the tokened fix only *added*
resurrect for source-backed leaves. The soak retries this ABORTED and passes; stateless (no retry) surfaces
it — the same production-robustness gap, on the DETACH/freeze path.

## Root cause (confirmed)

`Build::promote` runs the tokenless copy-forward **pre-pass** (`CasBuild.cpp:~816–829`) BEFORE
`store->mutateShard(...)`. The pre-pass consults the shared retire view **without refreshing it** — it sees
whatever round the view currently holds (R). Inside the `mutateShard` closure, the view is refreshed
conditionally: `if (store->retireView().round() < root.fence_round) store->retireView().refresh();`
(`CasBuild.cpp:~849–850`), advancing it to R′ ≥ `fence_round` and installing condemned tokens invisible
during the pre-pass. The in-closure blob revalidation (the bounded loop from the tokened fix) then checks
condemnation against the **post-refresh** view R′.

So any condemnation surfacing in `(R, R′]` — newly published by GC, or merely newly *visible* after the
mandatory refresh — is **missed by the single-shot, pre-refresh pre-pass**. The tokenless leaf is never
copy-forwarded, and the in-closure tokenless branch has no copy-forward, so it fails closed. When the
shared view is already at/above `fence_round` (pre-pass and closure see the same snapshot) there is no
discrepancy and no abort — the bug requires the closure's refresh to fire.

(`copyForwardFromCondemned` itself, `CasBuild.cpp:506`, is already a bounded, race-robust
GET→verify-payload-hash→re-wrap-fresh-incarnation→token-conditional-`putOverwrite` loop; the defect is
purely that the pre-pass runs it against a stale view and the closure has no fallback.)

## Design (Option A — in-closure copy-forward backstop; keep the pre-pass as fast path)

### 1. In-closure copy-forward backstop (the fix)

In the in-closure revalidation loop (`CasBuild.cpp` ~907–943), in the **condemned** branch, before the
current `!src → ABORTED`: if there is no retained source BUT the leaf is a **copy-forwardable tokenless
dep**, copy it forward instead of aborting, then re-check (the existing bounded loop re-HEADs):

```
if condemned:
    if src:                      uploadFromSource(...)                 // tokened (existing)
    else if isCopyForwardableTokenless(e.blob_hash):
                                 copyForwardFromCondemned(e.blob_hash, blob_key, hr)   // NEW
    else:                        throw ABORTED "condemned … (INV-1)"    // unknown leaf / tokened-source-lost
    continue
```

This runs **after** the in-closure refresh (`~849–850`) and **after** the owner-liveness check
(`~860–893`), so it (a) evaluates against the correct post-refresh view — the only place that can — and
(b) only resurrects when this build's precommit is the confirmed live owner (no orphan on an aborting
path). `copyForwardFromCondemned` mints a fresh `incarnation_tag` not in the fixed in-closure snapshot, so
the next HEAD validates — ≤2 iterations per leaf, same bound as the tokened case.

**The absent branch is unchanged and stays fail-closed:** an absent tokenless leaf (deleted, no source)
genuinely cannot be recreated — `copyForwardFromCondemned` requires a present object to GET (it aborts on
absent, `CasBuild.cpp:520–523`), and the loop's absent-`!src` branch keeps its `ABORTED`. Only
**present-condemned** tokenless deps are copied forward.

### 2. Single shared predicate (no drift)

Factor the classification into ONE helper so the pre-pass and the backstop cannot diverge:

```cpp
/// A leaf is copy-forwardable iff this build holds a TOKENLESS W-EVIDENCE dep for its hash
/// (adoptEvidence — an independent live committed owner exists; the INV-1 copy-forward exception applies).
/// A tokened dep, or NO dep at all (a staging bug — must fail closed), is NOT copy-forwardable.
bool Build::isCopyForwardableTokenless(const UInt128 & hash) const;
```

Both the pre-pass (`~816–829`) and the new backstop call it. **Unknown leaf (no dep) + condemned ⇒ still
ABORTED** — a manifest entry with no recorded dep is a staging bug and must fail closed, never silently
copy-forward.

### 3. Keep the pre-pass (fast path), optionally refresh once before it

Do NOT remove the pre-pass. The "GET+PUT don't belong in a retried closure" rationale is partly void (the
tokened `uploadFromSource` already PUTs in the closure), but a **different, valid** reason stands: the
flush holds `view_gate` shared across the whole closure and serializes the shard's flat-combining leader
(`CasStore.cpp`), so a large-blob copy-forward (full GET + full PUT) in the closure head-of-line-blocks
co-batched refs on that shard and holds `view_gate` against the retired-view install drain (which takes it
exclusively). So the pre-pass stays the **outside-the-lock fast path** for the common case; the in-closure
copy-forward is the **rare-race correctness backstop**.

Optionally add a single `store->retireView().refresh()` before the pre-pass loop to *narrow* the race (the
pre-pass then reliably does the heavy work outside the lock in more cases). This does NOT close the race
(GC can still advance between that refresh and the closure's `fence_round`-gated refresh), so it is a
fast-path optimization only — the in-closure backstop remains mandatory. (Optional; may be deferred.)

## Invariants / correctness

- **INV-1 (never revive by reading a dying object):** the copy-forward reads the condemned-but-present
  object — the documented, narrow exception, valid ONLY for tokenless `adoptEvidence` deps whose blob has
  an independent **committed** source owner (`ContentAddressedTransaction.cpp:184, 759–771`). It
  re-verifies the payload hash against the content key before republishing a byte
  (`CasBuild.cpp:538–543`), and every lost-race/absent/corrupt mode fails closed. No new INV-1 surface.
- **INV_NO_DANGLE:** the copy-forwarded blob has in-degree > 0 (the committed source manifest's edge AND
  the detached precommit's activating +edge, which the promote fold barrier guarantees is folded before the
  promote folds). We reach the backstop only past the owner-liveness check, which certifies this precommit
  is the live owner. The one sharp window (source-drop folds before the detached +edge folds → the blob is
  momentarily d=0 and correctly condemned, then revived for a live about-to-commit owner) is the **same**
  accepted window the tokened `uploadFromSource` resurrect already lives with — no new hole. Tie a comment
  to the fold-barrier (`CasBuild.cpp:~945–949`) + owner-liveness (`~860–893`) invariant: if either is ever
  weakened, this copy-forward would become resurrection-of-dead-data.
- **Idempotent under CAS retry:** `mutateShard` may re-invoke the closure; `copyForwardFromCondemned` is
  idempotent — on re-run, HEAD sees the fresh token (validate, no second copy-forward) or, if still d=0
  condemned, another bounded token-conditional `putOverwrite` displacing exactly the observed incarnation.
  RootShard edits roll back per-closure; only content-addressed, token-conditional object-storage effects
  persist. The GET+PUT-in-closure objection is cost/head-of-line only, not correctness.
- **Termination:** per-leaf ≤2 iterations (fresh incarnation ∉ fixed snapshot); `copyForwardFromCondemned`
  internally bounded (8); `mutateShard` CAS bounded. No unbounded spin.

## TLA+

The tokenless evidence copy-forward + publish gate is modelled in `CaIncarnationCore.tla` via the
`WEvidence` / `WResolveEvidence` actions (tokenless evidence deps) and the publish gate requiring children
`~CondemnedAtView` at the writer's view. Task 1 confirms the existing config that exercises evidence
resolution under condemnation holds `INV_NO_DANGLE` / `INV_NO_LOSS` / `INV_NO_RETURN` (run it; if none
exercises a condemned *evidence* dep resolved before publish, add a minimal config that does). The fix
brings the promote implementation into line with the modelled resolve-then-publish path (the impl was
stricter — the pre-pass missed the refreshed condemnation and aborted). No new constant expected.

## Testing

- **gtest (unit, RED→GREEN)** in `gtest_ca_wiring.cpp` (or `gtest_cas_protocol_scenarios.cpp`): stage a
  build that **adopts a blob via `adoptEvidence`** (tokenless dep, no retained source) from a committed
  source manifest; condemn that blob's token such that the condemnation is only visible after the
  in-closure fence refresh (seed the retired set + fence the namespace at a round ahead of the current
  view, mirroring `RevalidateAbsent…`/`FenceConflict…` setups); then `promote` and assert it **succeeds**
  (the leaf is copied forward to a fresh live incarnation) — RED against current code (ABORTED), GREEN with
  the fix. Second case: an **absent** tokenless leaf (deleted, no source) still ABORTs. Third: an
  **unknown leaf with no dep** that is condemned still ABORTs (fail-closed staging-bug guard).
- **Stateless:** `03283_optimize_on_insert_level` passes without retry on the CA-s3 lane.
- No `sleep`-based synchronization (CLAUDE.md).

## Files

- `Core/CasBuild.h` — `isCopyForwardableTokenless` decl.
- `Core/CasBuild.cpp` — the helper def; the in-closure backstop in the revalidation loop's condemned
  branch; route the pre-pass through the same helper; optional pre-pass refresh; the invariant comment.
- TLA+ — confirm/extend the evidence-copy-forward-under-condemn config.
- `gtest_ca_wiring.cpp` (or `gtest_cas_protocol_scenarios.cpp`) — the three cases above.
