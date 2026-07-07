---
description: 'Design for fixing RESURRECT-REUPLOAD-ORPHAN — a content-addressed GC leak where a resurrect-replaced blob incarnation is never re-condemned. Fold-time re-condemn keyed on the current incarnation token.'
sidebar_label: 'Resurrect-reupload-orphan fix'
sidebar_position: 50
slug: /superpowers/specs/cas-resurrect-reupload-orphan-fix
title: 'CAS GC — resurrect-reupload-orphan fix design'
doc_type: 'guide'
---

# CAS GC — resurrect-reupload-orphan fix design {#resurrect-reupload-orphan-fix}

## Problem {#problem}

Under recurring-hash create/insert/DROP churn the content-addressed GC leaks a small, bounded residual
of blob objects that `fsck` classifies `unaccounted` (INV-2: "outside the whole GC view — should be
impossible once GC has run"), sometimes `unreachable`. `dangling` stays 0 throughout, so there is no
committed reference to a missing object — this is a **liveness/space leak**, not a data-loss bug. Found
2026-07-07 in the `utils/ca-soak` S30 scenario, root-caused via `system.content_addressed_log`.

### Mechanism {#mechanism-of-the-leak}

In-degree is tracked **per content hash** (owner edges `(blob_hash, source_id)`), while condemn/retire
records the **exact incarnation token**. A blob's lifecycle under churn:

1. Token A is uploaded, referenced, then dereferenced → in-degree 0 → the fold condemns token A
   (`RetiredEntry{hash, A}`), which enters the two-phase graduation pipeline.
2. A new build dedup-hits the same content while A is still condemned-and-present. Per INV-1 it must not
   revive the condemned bytes, so it **re-uploads a fresh incarnation, token B, at the same blob key**
   (`blob_reuse_resurrect` → `blob_put`). B overwrites A at the key.
3. B is referenced then dropped.
4. GC's exact-token delete of A runs: it finds token B at the key and fail-safe-skips (`outcome=replaced`),
   dropping A's entry.
5. **B is never condemned.** `closeBlob` keyed the "already retired?" decision on the *hash*: while A's
   entry existed it took the settle branch and never reached the fresh-condemn path for the current
   token; and the fold is touch-gated (it only visits a hash with edge deltas this window). Once B's
   edges are folded, no later fold revisits B and no entry names token B → B orphans forever.

Root cause site: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp`,
the `closeBlob` / `settleEntry` logic (~L225–251). Reproduced and the fix validated in TLA+
(`docs/superpowers/models/CaGcResurrectReuploadOrphan.tla`).

## Fix {#fix}

Change the fold's condemn decision to key on **`(hash, current token)`** rather than on the hash — which
is exactly what the canonical model's `GRetire` already does. Expressed as one unified rule in
`closeBlob`:

> When a hash is **`touched`** this fold window (had owner-edge deltas — i.e. a reference/dereference
> cycle) and its net **`cur_edges == 0`**, `head_blob` the current token and **ensure the retired list
> holds a condemn entry for the CURRENT token**, superseding any entry of the same hash that names a
> stale (resurrect-replaced) token.

- If the current token equals the existing entry's token (a re-reference *adopted* the same live
  incarnation), the entry already exists → idempotent, normal graduation.
- If it differs (a resurrect replaced A with B), **replace** A's entry with `RetiredEntry{hash, B,
  condemn_round = this round}`. One entry per hash (the retired list is one-entry-per-hash). A's
  abandoned entry leaks nothing: B overwrote A at the key, so A's exact-token delete would only have
  found B and no-op'd ("replaced") anyway.

The re-condemn entry is produced inside the retire-merge (in hash order, so the release-gate
`is_sorted` invariant is preserved) and is committed by the existing **R5 single round CAS**
(`gc/state.retired_refs`). There is **no** change to the writer owner-edge journal, no delete-time
grafting, and no reading of `retired_refs` outside the fold.

### Observability {#observability}

A new CA-log event `blob_retire_replaced` (a `blob_retire` variant, keeping the "retire" verb) records
`{hash, old_token, new_token, round}` so `system.content_addressed_log` shows the repair. A profile
counter `CasGcRetireReplaced` increments per re-condemn.

## Safety {#safety}

- **`cur_edges == 0` ⟹ the current token is unreferenced** (no committed owner edge names the hash) —
  the same precondition the existing fresh-condemn uses. A live/published resurrect has `cur_edges > 0`
  → the existing **spare** branch keeps it (never condemned). So no live incarnation is condemned.
- **In-flight resurrect** (uploaded, precommit, not yet a committed owner): in-degree 0, so it *could*
  be condemned — but this is identical to what the fold already does when it condemns any in-degree-0
  blob (the writer's publish gate then rejects the condemned dep and it re-uploads). No new class of
  behavior; the starvation-vs-heartbeat question remains the `CaResurrectLiveness` /
  `CaBuildRootPrecommit` domain.
- **`everEdged` + present gates stay:** condemn still requires the hash to have been referenced at least
  once and the object to be present (`head_blob(...).exists`). A never-referenced fresh upload is not
  condemned.
- **Idempotency under R5-retry:** the decision is a pure function of `(this fold's edge stream, HEAD of
  the current token)`; a lost R5 CAS re-runs the round on the same durable journal + same object → same
  entry. The rule is upsert-shaped: an existing entry for the current token is left as-is (its
  `condemn_round` is not reset), so graduation is never stalled and no duplicate entry appears.

## Writer impact {#writer-impact}

`retired_refs` is consumed by writers via `store->retireView().isCondemnedToken(hash, token)` (≈8 sites
in `CasBuild.cpp`) to decide resurrect-vs-adopt and to validate dependencies. Adding `(hash, B)` makes a
dedup-hit on the hash correctly **resurrect** (fresh re-upload) instead of adopting the being-reclaimed
B. This *fixes* a latent hazard the current bug has: today only A is listed, so a writer dedup-hitting
the hash sees B as not-condemned and **adopts the orphan** — building on a GC-untracked, being-reclaimed
incarnation. `condemn_round` / `delete_pending` remain GC-only (writers ignore them).

## Cost {#cost}

- **`+1 HEAD` strictly per touched-condemned transition** (= reference/dereference cycle over a
  condemned hash = the resurrect rate). Quiescent graduations go through `settleRetiredBelow` (the hash
  is not in this window's edge stream) and take **no** HEAD, unchanged. This is structural: `closeBlob`
  (where the HEAD lives) is only entered for hashes in the edge stream.
- **No extra write** — the re-condemn `RetiredEntry` rides the existing R5 round CAS.
- **Removes** a wasted operation — today's exact-token delete that finds "replaced" and no-ops.

Net: ≈0 extra S3 ops in normal (unique-content) workloads; bounded by the resurrect rate under
recurring-hash churn; near-neutral-to-positive because it also eliminates the space leak.

## Testing {#testing}

TDD, unit-first, in `src/Disks/tests/gtest_cas_gc_leak.cpp`:

1. **RED — `ResurrectReplacedIncarnationReclaimed`:** build + reference + drop payload `P` (condemn
   token A); resurrect via a fresh `startBuild → putBlob(idOf(P), BlobSource::fromString(P))` on the
   condemned token (re-upload token B); reference + drop B; `runGcToFixpoint`. Assert
   `blobPresent(P) == false`, `inDegreeOf(P) == 0`, and `runFsck` shows `reachable/unreachable/
   unaccounted/dangling` for `P` all clear. Fails today (B orphaned).
2. **Implement** the `closeBlob` rule → GREEN.
3. **Idempotency:** drive extra rounds after reclaim; assert no re-condemn churn and no duplicate
   retired entries.
4. **Writer-side (nice-to-have):** after condemn A + resurrect B,
   `retireView().isCondemnedToken(hash, B) == true`.
5. **Scenario regression:** the deterministic S30 repro in `utils/ca-soak` must lose its `unaccounted`
   residual and pass.

## TLA+ {#tla}

The fix rule equals the canonical `CaIncarnationCore` `GRetire` keying (`(hash, current token)`), which
already proves the correct algorithm. The gate for this fix is the focused model
`docs/superpowers/models/CaGcResurrectReuploadOrphan.tla`: the `_bug.cfg` violates `NoLeakForever`, the
`_fix.cfg` holds. Refinement: align the model's fix branch to condemn the current token on
`touched && in-degree==0` (upsert keyed on the current token) so it mirrors the shipped `closeBlob`
change exactly. A faithful full-interleaving reproduction in `CaIncarnationCore` requires adding a
touch-gating dimension to `GFold`/`GRetire` (see `docs/superpowers/cas/06-tla-models.md` §Area 12) and
is a documented follow-up, not a blocker for this fix.

## Out of scope {#out-of-scope}

- Adding the touch-gating dimension to the canonical `CaIncarnationCore` model (follow-up).
- The `ca-gc-dryrun` `previewDeletes` reachability alignment (separate F3 item, already handled by the
  scenario oracle fix).
- The graduation-drain / `mount_renew_period` cost tuning for the soak harness (separate backlog item).

## Docs to update after the fix lands {#docs-to-update}

Once implemented and verified, update: `docs/superpowers/cas/06-tla-models.md` §Area 12 (mark the C++
fix landed; the model becomes a regression gate rather than a pending-design gate); any CAS GC design
doc that describes the condemn/retire keying (state that condemn keys on `(hash, current token)`, and
that a resurrect-replaced incarnation is re-condemned in the fold that folds its dereference); and
`utils/ca-soak/scenarios/BACKLOG.md` `RESURRECT-REUPLOAD-ORPHAN` (mark resolved once S30 goes green).
