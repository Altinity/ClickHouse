---
description: 'Index of every TLA+ formal model in the CAS MergeTree corpus: what each proves, the counterexamples and sabotages that drove design decisions, and code-currency status.'
sidebar_label: 'CAS TLA+ model index'
sidebar_position: 6
slug: /superpowers/cas/tla-models
title: 'CAS MergeTree — TLA+ model index'
doc_type: 'guide'
---

# CAS MergeTree — TLA+ model index {#cas-tla-model-index}

This document indexes every TLA+ model in `docs/superpowers/models/` for the content-addressed (CAS)
MergeTree feature. For each model it records: the file(s), what the model proves (key invariants), the
counterexamples or sabotage traces that drove concrete design decisions, and a code-currency note.

The `.tla` and `.cfg` source files are **not modified**; this doc superseded the older per-model prose
files (`*_README.md`, `INDEX.md`, `RESULTS.md`, `MODEL_CURRENCY_REVIEW_2026-06-22.md`). All
bounded-model-checking runs use TLC v2.19 / OpenJDK 21; Apalache 0.58.0 is used for
inductive-invariant checking.

> **Directory index.** The complete, up-to-date inventory of `docs/superpowers/models/` — including
> models added after this doc's last full refresh (the ref snapshot+log family `CaRef*`,
> `CaRetiredInRun*`, `CaGcCondemnMarkerGate`, and the rev.6 `CaCasMountCore` configs) and the list
> of removed obsolete models — is `docs/superpowers/models/README.md`. When this doc and the README
> disagree on a model's config set or currency, the README (audit date 2026-07-21) wins.

---

## Area 1 — Incarnation-token GC core {#area-incarnation}

The canonical model of the fold → retire → fence → recheck → exact-token-delete → cascade → trim round.
Replaces the superseded EBR/epoch design (`CaGcCore.tla`; see §Area 8).

### `CaIncarnationCore.tla` — canonical GC core {#caincarnationcore}

**Files:** `CaIncarnationCore.tla`, `CaIncarnationCore_stage*.cfg`, `CaIncarnationCore_sab_*.cfg`,
`CaIncarnationCore_hunt_*.cfg`, `CaIncarnationCore_reval_stage2.cfg`

**What it proves.** The safety invariants `INV_NO_DANGLE` (no manifest ref points to an absent object),
`INV_NO_LOSS` (every reachable object stays present), `INV_NO_RETURN` (a present object's token is
never in the deleted-token history `deadTok`), and `INV_JOURNAL_COVERAGE` (the trim base never advances
past the fold cursor) hold across the full adversarial interleaving: concurrent writers and GC leaders,
split-brain, debris classification, full-GC exact-cut, `WResurrect`/`WOverwrite`, tree expansion and
atomic cascade, namespace registry + manifest creation (B91), evidence staleness + re-observation (B91).

| Stage | Configs | Adds | Distinct states | Result |
|---|---|---|---|---|
| 1 core | `stage1` | publish/drop, fold/retire/fence/recheck, in-flight deletes | 20.9M | PASS |
| 2 resurrect/evidence | `stage2` | `WResurrect`, `WEvidence`, `WResolveEvidence` | 155K | PASS |
| 3 trees/cascade | `stage3` | expansion markers, atomic cascade, shared-child survival | 52.8M | PASS |
| 4a debris/full-GC cut | `stage4_small` | heartbeat-gated debris, two-shard full-GC cut | 16.8M | PASS |
| 4b journaled-delete + tree rebuild | `stage4_journaltree` | journaled tail + `FGCommit` tree rebuild | 35.6M | PASS |
| 5 split/overwrite | `stage5_small` | split-brain + `WOverwrite` | 64.4M | PASS |
| 6a registry | `stage6_registry` | namespace registry + manifest creation (B91) | 3.7M | PASS |
| 6b evidence staleness | `stage6_evstale` | amended `W-EVIDENCE`, `WEvObserve` (B91) | 10.1M | PASS |
| 6c cross smoke | `stage6_cross_smoke` | registry × evidence-staleness interaction | 440K | PASS |
| liveness | `stage2_live` | `NoLeakForever` under `FairSpec` | — | bound-artifact lasso (MaxRound=2 budget, not a design bug) |

Large-bound hunt (2026-06-11, `W-REVALIDATE` mode): 782M distinct states BFS + 8.3B deep random
visits at enlarged bounds — 0 violations found.

**Sabotages (negative controls — all must produce a counterexample):**

| Config | Rule removed | Violated invariant | Trace summary |
|---|---|---|---|
| `sab_nofence` | fence does not write to manifests (horn 2 missing) | `INV_NO_DANGLE` | Post-fence publish not blocked → GC deletes the committed object |
| `sab_norecheckfold` | recheck does not require fold-through-fence (horn 1 missing) | `INV_NO_DANGLE` | Pre-fence publish missed by the recheck cursor → object deleted before the late publish is folded |
| `sab_noretireview` | W-PUBLISH-GATE retire-view check removed | `INV_NO_DANGLE` | Stale reuse dep re-published after the fence; GC deletes the object under the stale ref |
| `sab_unconddelete` | exact-token delete replaced by unconditional delete | `INV_NO_DANGLE` | Stale delete message kills the live token-2 incarnation written by `WResurrect` |
| `sab_reusedtag` | W-FRESH-TAG / token distinctness removed | `INV_NO_RETURN` | Resurrect reuses the condemned token-1; delete lands, marks token-1 dead; incarnation still present |
| `sab_cascade` | cascade deferred as a separate pipeline step | `INV_NO_LOSS` | Round-2 GC re-expands a now-live tree's stale `pendCasc` strip, deleting a live child |
| `sab_cutoverclaim` | full-GC cursor jumps past unincorporated incorporated-state | `INV_NO_DANGLE` | `FGCommit` skips the add record; recheck fires as if the fold covered it; delete lands |
| `sab_noreobserve` | W-REVALIDATE re-observation conjunct removed (reval mode) | `INV_NO_DANGLE` | Stale dep on a deleted object passes the weakened publish gate |
| `sab_noregistry` (B91) | namespace registration skipped; no publish-floor | `INV_NO_DANGLE` | Newborn namespace's publish floor stays 0; a publish lands before the registry fence captures it |
| `sab_foldtimeuniverse` (B91) | GC fences the FOLD-TIME registry universe instead of commit-time | `INV_NO_DANGLE` | A namespace registered between the fold's read and the registry-fence CAS is never fenced → its exact-token delete dangles |
| `sab_noevreobserve` (B91) | stale evidence admitted without re-observation | `INV_NO_LOSS` | A stale carry-forward dep on a deleted object reaches publish; bytes lost |

**Design decisions driven by this model:**

- The fence writes to EVERY manifest (not just manifests with activity) — established by `sab_nofence`.
- Recheck requires the fold cursor to have advanced through the fence position — established by `sab_norecheckfold`.
- Deletes are exact-token — established by `sab_unconddelete`.
- Cascade is atomic with the delete landing (not a deferred pipeline step) — established by `sab_cascade`.
- The registry fence must use the COMMITTED (not fold-time) universe — found as a real C++ hole
  (`commit 724eb5363ff`) during the B91 refresh; `sab_foldtimeuniverse` is the permanent negative
  control for this shape.
- `adoptTree` must cold-reuse (`observeAndAdmit`), not accept tokenless evidence — found as a real C++
  blind-adopt hole (`commit a247e29c125`) during the B91 refresh.
- The `GFenceShard` fence write must be a monotone-max CAS (never lowers the durable fence under
  split-brain) — model refinement MR-4.

**Model-refinement findings for the publish gate (MR-1 / MR-2·F1 / MR-3·F2):** three refinements the
model forced on the writer's publish gate. The literal names below are in `CaIncarnationCore.tla`.

- **MR-1 — publish gate consults durable `deadTok[h]`, not only in-flight `retired`.** A
  token-bearing dependency `(h, t)` is condemned at view `v` iff
  `CondemnedAtView(h, t, v) == RetiredHit(h, t, v) ∨ t ∈ deadTok[h]`. Consulting only the live
  `retired` set is unsafe: the `Land` action consumes the retired entry on delete, so after the
  message lands a stale unpublished dependency on `(h, t)` would pass `DepOK` even though the token
  is physically dead. `deadTok[h]` is the durable history of tokens that have stopped being current.

- **MR-2 / F1 — displaced-token push + current-state re-validation.** Any action that makes a token
  stop being current MUST push the displaced token into `deadTok[h]`: in-place overwrite
  (`WResurrect`, `WOverwrite`) and physical delete (`Land`). Correspondingly, in `W-REVALIDATE` mode
  (`EnableReval`) the publish gate re-validates a dependency's **current physical state**
  (`present[d] ∧ tokOf[d] = observed_token`) in the same CAS, not merely the originally-observed
  token. Negative control: `sab_reusedtag` re-issues a condemned token and violates `INV_NO_RETURN`;
  `sab_noreobserve` drops the re-observation conjunct and dangles.

- **MR-3 / F2 (`TreeDepsOK`) — bottom-up tree publish.** A tree ref may be published into a manifest
  only when **all direct children are present and non-condemned at publish time**
  (`TreeDepsOK(w, h) == h ∈ TreeHashes ⇒ ∀c ∈ Children[h] : present[c] ∧ ~CondemnedAtView(c, tokOf[c], wView[w])`,
  called from `WPublish`). Publishing a tree over an absent/condemned child immediately dangles once
  the fold (`GFold`) expands the tree edges. This is one-level closure in the model (nested subtrees
  not modeled); the writer's bottom-up build discipline covers the transitive case in code.

**P9 extension (2026-06-17):** `GForget` prunes absent zero-in-degree nodes from `everEdged` to
eliminate the retire-404-HEAD storm. Frame argument: no safety invariant reads `everEdged`, so
`GForget` preserves all invariants independent of scope. Re-verified at stages 1–3 (clean). Liveness
lasso unchanged.

**Code currency:** CURRENT as the **architecture-independent safety spine**; its concrete structure
is superseded (audited 2026-07-22). Every proven guard maps to a faithful code counterpart —
incarnation token + exact-token delete + `INV_NO_RETURN` (`CasGc.cpp:419,507-512`), fold → retire →
fence → recheck → delete → cascade → trim, no destructive decision on a not-fully-folded snapshot
(realized by `suppress_destructive`), retired entries drop only on a confirmed outcome — so no proven
invariant is at risk. **But its concrete representation is superseded by the `cas-gc-rebuild`
architecture**: the model embeds the journal *inside the manifest* (`man[s].log`) and appends the
fence as a *record in that journal* gating writers on `man[s].fence`, whereas the shipped code uses
separate ref-log objects (`RefTxnId`), source-edge runs, write-once fold seals, and heartbeat-floor
fencing — there is no manifest-embedded journal or manifest fence record. So read this model as the
canonical *invariant* proof, not a structural mirror of the code (the concrete layer is tracked by
`CaGcRootLocalPartManifestCore.tla` + the ack-floor/round-defer models). Other standing discrepancies:
(1) heartbeat/debris/full-GC-cut actions (`WHb*`, `GDebrisRetire`, `FGRead`, `FGCommit`) are specified
in the model but deferred in code; (2) `CaBuildRootPrecommit.tla` is the live precommit safety
mechanism, only obliquely modeled here; (3) the `EnableReval=FALSE` dead-token-oracle gate has no
production code path.

---

### `CaIncarnationProofCore.tla` — Apalache inductive invariant {#caincarnationproofcore}

**Files:** `CaIncarnationProofCore.tla`, `Apalache.tla`, `CaIncarnationProofCore_tlc.cfg`,
`CaIncarnationProofCore_tlc2h.cfg`, `run_apalache.sh` — **removed 2026-07-22** (stale AND
unverifiable here — no Apalache binary; full text in git history). It modeled a superseded fragment
of the core and could not be re-checked, let alone re-derived, in this environment. If an inductive
proof is wanted, install Apalache and re-derive `IndInv` against the current `CaIncarnationCore.tla`.

**What it proved.** An inductive invariant `IndInv` (19 conjuncts) for the pre-B91, `W-REVALIDATE`
token-only fragment (single leader, no trees/debris/evidence/split-brain). Apalache 0.58.0 verified
base case (`Init => IndInv`, 1 s) and step check (`IndInv => IndInv'`, 45–73 s) — both `NoError`. This
is stronger than TLC bounded-checking: the step check quantifies over ALL states satisfying `IndInv` at
fixed constant sizes (`|Writers|=2`, `|Shards|=1`, `|Hashes|=2`, `MaxToken=3`, `MaxRound=2`,
`MaxLog=4`), regardless of trace depth. The prepared CTI journal (12 iterations) is the input for a
future parametric TLAPS proof.

The heart conjunct is `InflightCurrentUnreferenced` (a delete message in flight for a still-current
token implies no folded root edge and no unfolded add past the cursor). Found as irredundant by the
negative control `IndInv_NoICU`; dropping it produces a CTI.

Negative controls: dropping `InflightHeld` or `InflightVsRefs` each leave the step check green
(documented redundancy — both are corollaries of ICU + the fence-discipline family). Removing the
`W-REVALIDATE` re-observation conjunct from `WPublish` (`NextNoReval`) produces a counterexample
breaking `NoDangle` — machine-checking the F1 re-observation requirement.

**Code currency:** STALE and REMOVED 2026-07-22. Predated the B91 amendments (namespace registry,
evidence staleness, `ViewableRound`); the canonical current-design coverage is
`CaIncarnationCore.tla` (bounded TLC, CURRENT). A stale inductive proof of a superseded fragment
that cannot be re-verified in this environment is false comfort, so it was removed rather than kept
as speculative groundwork.

---

## Area 2 — Build-root / precommit protection (B140, B171, B199-S2) {#area-precommit}

### `CaBuildRootPrecommit.tla` — precommit-first + fail-closed commit {#cabuildRootPrecommit}

**Files:** `CaBuildRootPrecommit.tla`, `CaBuildRootPrecommit_buggy.cfg`,
`CaBuildRootPrecommit_buildrootonly.cfg`, `CaBuildRootPrecommit_failclosedonly.cfg`,
`CaBuildRootPrecommit_fixed.cfg`, `CaBuildRootPrecommit_inlineclosure.cfg`,
`CaBuildRootPrecommit_lazyleak.cfg`, `CaBuildRootPrecommit_inlineclosure_b2.cfg`,
`CaBuildRootPrecommit_b2_witness.cfg`

**What it proves.** The 2×2 necessity/sufficiency matrix for the B140/B171 fix — build-root structural
reachability (`UseBuildRoot`) and fail-closed commit (`FailClosedCommit`) — and the B199-S2
inline-closure liveness fix (`InlineClosure`).

| `UseBuildRoot` | `FailClosedCommit` | `InlineClosure` | Spec | Result | States |
|---|---|---|---|---|---|
| F | F | T | `Spec` | `INV_NO_DANGLE_COMMITTED` violated | ~505 (to CE) |
| T | F | T | `Spec` | `INV_NO_DANGLE_COMMITTED` violated | ~3031 (to CE) |
| F | T | T | `Spec` | clean (safety) | 2193 |
| T | T | T | `Spec` | **clean** (all 4 invariants) | 45161 |
| T | T | T | `FairSpec` | **clean** (safety + `INV_NO_LEAK` HOLDS) | 45161 |
| T | T | F | `FairSpec` | `INV_NO_LEAK` **VIOLATED** — S2 leak reproduced | 71953 (to CE) |
| T | T | T (b1+b2) | `FairSpec` | **clean** (safety + `INV_NO_LEAK` HOLDS) | 310993 |

**B140-dangle counterexample (buggy cfg, 6 states):** `WriteBlob(bld1,b1)` → `AdoptBlob(bld2,b1)` [owner stays `bld1`] → `BuildDie(bld1)` [owner gone, `OwnerProtected(b1)=FALSE`] → `GcDelete(b1)` [in-degree 0, unprotected] → `Commit(bld2,t1)` [no presence re-check, blind publish] → **committed manifest references absent `b1`**. This is adopt-without-ownership-transfer → owner retires → GC deletes → blind commit dangles.

**Why build-root alone is insufficient (buggy `buildrootonly` counterexample):** `WriteBlob(bld1,b1)` → `GcDelete(b1)` (no precommit yet, so in-degree 0) → `Precommit` → blind `Commit` → dangle. The precommit must exist **before** the blob can reach in-degree 0 AND the commit must re-check presence.

**B199-S2 inline-closure leak (`lazyleak`, 6 states + stutter):** `Precommit(bld2,t2)` while tree object `t2` **absent** → lazy path records `closure[bld2]={}` (empty) → `b1` never enters `everSnapped` → `GcDelete` can never fire on `b1` → `b1` leaks forever. Inline closure (IC=TRUE) closes this by construction: the writer records the closure at precommit time, from the staged structure it already holds in memory.

**Reachability witnesses (non-vacuity):** all four negated witnesses are reachable: `W_GcDeleteReached`, `W_BuildRootProtectReached`, `W_LiveFrozenReclaimDeleteReached`, `W_PrematureReclaimAbortReached`. The two-blob run confirms shared blobs are spared and unique blobs are reclaimed.

**Design decisions driven by this model:**

- Build-root reachability (structural, not revocable liveness hint) is required — adopter builds must
  precommit before relying on the adoption.
- Fail-closed commit (presence re-check on the whole closure) is independently necessary — it catches
  the ordering-window case where the blob is deleted before the precommit edge exists.
- Both halves are necessary and jointly sufficient.
- The precommit must record its closure **inline at precommit time** — not lazily from a tree-object
  read at GC fold time (which can 404 if the tree is already condemned/deleted).

**Code currency:** CURRENT for the safety conclusion; two named mechanisms have DRIFTED (audited
2026-07-22) — the model's proven conclusion (build-root reachability + fail-closed commit jointly
close the adopted-blob dangle) still holds, but the code realizes it differently than the two
bullets above describe:

- The "record its closure **inline at precommit time**" bullet is **not** how the shipped writer
  works. `precommitAdd` carries only the manifest ref, never a blob list
  (`CasPartWriteTxn.cpp:879-890`); GC learns the closure by reading the manifest body **lazily at
  fold time** (`foldManifestEdges`, `CasGc.cpp:789-796`). Shipped closes the same leak by a
  different, unmodeled discipline: a missing-body `+1` fold **clamps** the per-table cursor (a
  barrier that re-reads next round) instead of recording an empty closure, and GC is the **sole
  deleter** of the manifest body, deferring its exact-token delete until after the `-1` edges fold
  (`mf_cleanup`, `CasGc.cpp:815-816`). So `InlineClosure=TRUE` models a mechanism the code does not
  use; the lazy path it labels the S2 leak is the shipped path, made safe by the clamp barrier.
- "Fail-closed commit (presence re-check on the whole closure)" is realized as an
  **owner-liveness** fail-close, not a per-blob presence HEAD: `promote` aborts unless the precommit
  is still the live owner (`CasPartWriteTxn.cpp:963-968`); tokened leaves are skipped as
  edge-protected and tokenless leaves trusted. This is *stronger* (live owner ⇒ in-degree ≥ 1 ⇒
  present, GC being the sole in-degree-respecting deleter), so it is sound — but it is not the
  model's `∀c: present[c]` gate.

Not a CODE-RISK — the safety conclusion holds via these stronger/different mechanisms. The model
should be re-annotated (or its `InlineClosure`/presence-gate arms recast to the lazy-fold +
clamp-barrier + GC-owned-deletion + owner-liveness discipline) so it structurally mirrors the code;
that recast is a scoped follow-up, not done in this pass. The flat (one-level) model does not cover
nested subtrees; that recursion fix is validated by the C++ gtest in
`src/Disks/tests/gtest_cas_gc_leak.cpp`.

---

## Area 3 — GC lease heartbeat (B160) {#area-lease}

### `CaGcLeaseCore.tla` — GC leader lease / advisory heartbeat {#cagcleasecore}

**Files:** `CaGcLeaseCore.tla`, `CaGcLeaseCore_heartbeat.cfg`,
`CaGcLeaseCore_safety_noheartbeat.cfg`, `CaGcLeaseCore_sab_noheartbeat.cfg`

**What it proves.** `NoEpochCollision` (no two leaders commit a retire at the same fence epoch — the
atomic single-CAS steal + `fence_seq` epoch isolation is safe regardless of timing) and `NoFalseSteal`
(no steal fires against an alive mid-round incumbent when the advisory heartbeat is enabled).

| Config | `EnableHeartbeat` | Invariants | States | Result |
|---|---|---|---|---|
| `_heartbeat` | TRUE (fix) | `NoEpochCollision`, `NoFalseSteal` | 8,633 | PASS |
| `_safety_noheartbeat` | FALSE | `NoEpochCollision` | 10,777 | PASS |
| `_sab_noheartbeat` | FALSE (sabotage) | `NoFalseSteal` | 618 (to CE) | VIOLATED |

**B160 counterexample (`sab_noheartbeat`, 7 states):** `Tick, Tick` → `Create(L2)` (L2 mid-round, `seq` frozen) → `ObserveOrSteal(L1)` (records frozen obs) → `Tick` (full window; heartbeat OFF, `hb` frozen) → `ObserveOrSteal(L1)` (owner, seq, AND hb all frozen → L1 steals from the alive, mid-round L2 → `falseSteal=TRUE`). With heartbeat ON, the second `Tick` bumps L2's `hb`; L1 backs off.

**Design decisions driven by this model:**
- Safety is independent of the heartbeat — the epoch-fence CAS alone prevents double-commit.
- The advisory heartbeat is the minimal addition that eliminates false steals from a mid-round leader
  whose `seq` is frozen for the round's duration.

**Code currency:** CURRENT. Untouched by B171.

---

## Area 4 — Mount ownership and server-root identity {#area-mount}

### `CaCasMountCore.tla` — mount ownership safety gate {#cacasmountcore}

**Files:** `CaCasMountCore.tla`, `CaCasMountCore_stage1.cfg`, `CaCasMountCore_rev6_observe.cfg`,
`CaCasMountCore_sab_adoptwedge.cfg`, `CaCasMountCore_sab_epochreset.cfg`,
`CaCasMountCore_sab_fenceresurrect.cfg`, `CaCasMountCore_sab_foreigntakeover.cfg`,
`CaCasMountCore_sab_wallclockreclaim.cfg`, `CaCasMountCore_witness_reclaim.cfg`,
`CaCasMountCore_witness_observedreclaim.cfg`,
`CaCasMountCore_witness_recoveryafterobservedreclaim.cfg`,
`CaCasMountCore_witness_remountafterfence.cfg`

**What it proves.** The sticky-owner / durable-monotone-epoch / TTL-lease mount protocol over a shared
server-root, extended (2026-07-14) with the lease-boundary-exclusivity mechanism the shipped code
uses: `NoTwoServerUuidsOwnSameServerRoot` (owner is sticky), `ForeignUuidNeverAutoTakesOver` (mount is
never held by a non-owner), `WriterEpochMonotoneUnique` (the durable epoch counter is a monotone
ceiling, never reset), `GlobalSupersededWriterMakesNoMutation` (a fenced/superseded actor makes no
mutation — now a *knowledge* witness, not a per-write body-read guard), plus the observation-based
reclaim safety of the exclusivity extension.

The exclusivity extension is the load-bearing current mechanism: reclaiming an expired mount is
**observation-based** — the reclaimer must observe a *stable holder token* over a full `TTL + Drift`
window measured on its **own** monotonic clock (never trusting the wall-clock timestamp in the foreign
mount body), and the reclaim installs the **successor's own body**. This matches the shipped
`claimMountAwaitingExpiry` (`CasServerRoot.cpp:395-470`, threshold `ttl + ttl/20 + cadence` on
`mono_ms_fn` only) and the same-uuid/different-epoch reclaim that installs `makeMountBody(our_uuid,
our_epoch, …)` (`CasServerRoot.cpp:331-332`). The durable epoch is a separate CAS-bumped
`ServerEpoch` object that fails closed rather than reset (`allocateWriterEpoch`,
`CasServerRoot.cpp:151-195`). The per-write path is a **pure local** check
(`CasMountRuntime::mayMutate` = `!lost ∧ now < deadline`, `CasMountRuntime.cpp:62-66`) — the old
per-write epoch/body-read guard was correctly removed.

| Config | Kind | Invariant / witness | Result |
|---|---|---|---|
| `stage1` | positive | sticky-owner / epoch-monotone / superseded-no-mutation | PASS |
| `rev6_observe` | positive (main green gate) | observation-based reclaim safety | PASS |
| `sab_foreigntakeover` | sabotage | `ForeignUuidNeverAutoTakesOver` | VIOLATED |
| `sab_epochreset` | sabotage | `WriterEpochMonotoneUnique` | VIOLATED |
| `sab_wallclockreclaim` | sabotage — reclaim trusts the foreign wall-clock timestamp | reclaim-exclusivity safety | VIOLATED |
| `sab_adoptwedge` | sabotage | reclaim/adopt exclusivity | VIOLATED |
| `sab_fenceresurrect` | sabotage | fence-out exclusivity | VIOLATED |
| `witness_reclaim`, `witness_observedreclaim`, `witness_recoveryafterobservedreclaim`, `witness_remountafterfence` | reachability | the reclaim/fence-recovery states are reachable | VIOLATED (expected) |

**Design decisions driven by this model:**
- An expired-mount reclaim branch must still check `owner = uuid`, and must wait out `TTL + Drift` on
  the reclaimer's **own** clock observing a stable token — never trust the foreign body's wall-clock
  timestamp (`sab_wallclockreclaim` is the permanent negative control for that mistake).
- The reclaim installs the successor's own body, not a copy of the foreign one.
- The epoch object must never be reset; the durable counter is a strict monotone ceiling.
- The per-write guard is a pure-local liveness check; a superseded actor is blocked by latching
  `lost` (knowledge), not by a per-write shared-state read.

**Code currency:** CURRENT (model faithful; audited 2026-07-22). Note: the earlier
`sab_supersededwrites` config and the `SupersededWriterMakesNoMutation` per-write invariant were
retired when the per-write body-read guard was removed; the exclusivity extension (observation-based
reclaim, `Drift`, `sab_wallclockreclaim`, the `witness_observedreclaim*` / `witness_remountafterfence`
reachability checks) is the current mechanism above.

---

## Area 5 — B140 dangle: faithful reproduction and fix proof {#area-b140}

Three models formed a deliberate progression from initial reproduction to fix proof. The fix-proof
model (`CaB140DangleMerge.tla`) is the kept survivor; the two reproduction models were removed
during the 2026-07 model audit (sections kept as the record).

### `CaB140DangleMerge.tla` — faithful B140 reproduction + fix proof {#cab140danglemerge}

**Files:** `CaB140DangleMerge.tla`, `m_both_buggy.cfg`, `m_cursorskip.cfg`,
`m_trimonly.cfg`, `m_merged.cfg`

**What it proves.** The 2×2 necessity/sufficiency of the trim-gate + cursor-in-snap fix for the
trim-before-durable dangle across a GC lease handoff.

| `TrimGated` | `CursorInSnap` | Config | Result | States |
|---|---|---|---|---|
| F | F | `m_both_buggy` | `INV_NO_LOSS` violated | 0.71M (to CE) |
| T | F | `m_cursorskip` | `INV_NO_LOSS` violated | 0.85M (to CE) |
| F | T | `m_trimonly` | `INV_NO_LOSS` violated | 0.34M (to CE) |
| T | T | `m_merged` | **clean** | 5.33M |

**B140 counterexample (17 states):** L1 folds `add t2` (edge `t2→b1`) into in-memory wip → `GTrim` uses L1's in-memory cursor (trim-before-durable) → L1 loses lease, wip discarded → L2 rebuilds from empty committed snap → L2 GAP-skips trimmed `add t2` → `t2→b1` never enters any durable snap → L2 retires and deletes `b1` while live `t2` still references it.

**Cursor-skip counterexample:** `GCommitCursor` publishes cursor=1 while committed edges still point at empty gen-0 → `GTrim` (gated by committed cursor) trims `add t2` → the cursor ran ahead of the edges, so the gate over-trims. This shows trim-gate alone is insufficient: the cursor it trusts must be coherent with the committed edges, which is exactly what cursor-in-snap guarantees.

**Design decisions driven by this model:**
- The committed snap is one atomic write-once object carrying its own fold cursor (no separate `GCommitCursor` step independent of edge commit).
- The journal may be trimmed only up to the committed snap's cursor.
- Neither half alone closes the dangle; both are necessary and jointly sufficient.

**Code currency:** CURRENT (as history record). The live B140 protection model is `CaBuildRootPrecommit.tla`.

---

### `CaB140DangleFaithful.tla` — faithful refutation of Phase-1 mechanism {#cab140danglefaithful}

**Files:** `CaB140DangleFaithful.tla`, `CaB140DangleFaithful_shared.cfg` — **removed 2026-07-22**
(full text in git history).

**What it proved.** Clean over 9.1M states — the Phase-1 B140 fix (faithful producers: no marker-retaining strip, no field-mixed generation adoption) does not exhibit the dangle under the original Phase-1 mechanism. Superseded `CaB140Dangle.tla` as the faithful producer model.

**Code currency:** historical record only (a refutation about the long-gone Phase-1 mechanism); files removed 2026-07-22.

---

### `CaB140Dangle.tla` — Phase-1 B140 reproduction (superseded as producer) {#cab140dangle}

**Files:** `CaB140Dangle.tla`, `CaB140Dangle_adopt.cfg`, `CaB140Dangle_blob.cfg`,
`CaB140Dangle_loss.cfg`, `CaB140Dangle_producer.cfg`, `CaB140Dangle_safe.cfg` — **removed
2026-07-21** (full text in git history).

An initial Phase-1 reproduction with unfaithful producers (marker-retaining strip, field-mixed generation adoption). Superseded as a producer model by `CaB140DangleFaithful.tla`.

**Code currency:** SUPERSEDED as producer; files removed 2026-07-21.

---

## Area 6 — Build watermark and resurrect liveness (B167) {#area-watermark-resurrect}

These three models were **stale against shipped code** — they modeled a per-candidate blob-guard
(`protectedByLiveBuild`) that B171 removed and a condemn-time `HeartbeatGuard` never implemented
(deferred M-F Full GC). Their safety role fully migrated into `CaBuildRootPrecommit.tla`. All three
were **removed on 2026-07-21**; these sections stay as the record of the investigation and as
documentation of the B167 livelock shape.

### `CaResurrectLiveness.tla` — abstract resurrect-liveness (B167) {#caresurrectliveness}

**Files:** `CaResurrectLiveness.tla`, `CaResurrectLiveness_guard.cfg`,
`CaResurrectLiveness_noguard.cfg` — **removed 2026-07-21** (full text in git history).

**What it proves.** The abstract `HeartbeatGuard` boolean is load-bearing for resurrect liveness:
guard ON → `<>published` holds (4 states); guard OFF → livelock lasso (7 states).

**B167 livelock (noguard lasso):** `present=T, condemned=T` (dedup hit on stale incarnation) → `GcDelete` → `BuildUpload` (fresh incarnation, `freshOwned=T`) → `GcCondemn` (guard OFF: GC re-condemns the build's OWN fresh incarnation) → `GcDelete` → ... loop, `published` never TRUE. The key insight: the stale-incarnation upload→publish span is NOT atomic; GC can re-condemn in the gap.

**Design decision:** writer-side re-upload alone is starvable; a guard that blocks GC from condemning a freshly-owned incarnation is required.

**Code currency:** STALE vs shipped; files removed 2026-07-21. Modeled the deferred M-F `HeartbeatGuard` (condemn-time guard on live `build_id`). The shipped protection is precommit-first reachability (`CaBuildRootPrecommit.tla`).

---

### `CaBuildWatermark.tla` — concrete watermark oracle (B167) {#cabuildwatermark}

**Files:** `CaBuildWatermark.tla`, `CaBuildWatermark_guard.cfg`, `CaBuildWatermark_noguard.cfg`,
`CaBuildWatermark_staleactive.cfg`, `CaBuildWatermark_unsounddetect.cfg`,
`CaBuildWatermark_crash.cfg` — **removed 2026-07-21** (full text in git history).

**What it proves.** The concrete `min_active` scalar watermark oracle converges (safety: `Inv_ProtectedNeverCondemned`, `Inv_NoDangle` hold in all five configs; liveness: `<>(published=Builds)` holds with guard ON and crashes are leak-free). Three negative controls show the three independent failure modes each reproduce the B167 starvation lasso: no guard, stale active set (floor advances past in-flight builds), unsound crash detection (false-positive `gcDead`).

**Design decisions driven by this model:**
- `build_seq` must be allocated from a **monotone counter** (not just unique) — `CaBuildWatermarkNum` finding.
- The active-set floor (`min_active`) must be the exact minimum of in-flight build sequences.
- Crash detection must be sound (frozen-seq-across-K-passes discipline; false-positive death is fatal to liveness).

**Code currency:** STALE vs shipped; files removed 2026-07-21. The per-candidate blob-guard (`protectedByLiveBuild`) was removed by B171 (replaced by precommit-first reachability). The watermark floor lemma (`monotone build_seq`) survives for precommit-ref reclaim (`Gc::prefixEligible` / the `BuildPrefix` watermark floor in `CasGc.cpp`), not blob protection.

---

### `CaBuildWatermarkNum.tla` — numeric watermark validation {#cabuildwatermarknum}

**Files:** `CaBuildWatermarkNum.tla`, `CaBuildWatermarkNum_correct.cfg`,
`CaBuildWatermarkNum_confused.cfg`, `CaBuildWatermarkNum_nonmonotonic.cfg` — **removed 2026-07-21**
(full text in git history).

**What it proves.** Safety (`Inv_ProtectedNeverCondemned`, `Inv_NoDangle`) of the concrete numeric floor with two servers, real `epoch` watermarks, and real `build_seq` allocation. Key finding: monotone `build_seq` allocation is load-bearing — uniqueness alone is insufficient; a non-monotone allocation lets `min_active` be pulled back below a finished build's seq, re-protecting a condemned blob (a leak). The `_confused` config (wrong server's watermark) violates `Inv_ProtectedNeverCondemned`.

**Code currency:** STALE vs shipped (same as `CaBuildWatermark.tla`); files removed 2026-07-21. The monotone-`build_seq` floor lemma survives for precommit-ref reclaim.

---

## Area 7 — Root-local part-manifest GC (streaming sharded redesign) {#area-partmanifest}

### `CaGcRootLocalPartManifestCore.tla` — root-local part-manifest GC R0 gate {#cagcrootlocalpartmanifestcore}

**Files:** `CaGcRootLocalPartManifestCore.tla`, `CaGcRootLocalPartManifestCore_stage*.cfg`,
`CaGcRootLocalPartManifestCore_sab_*.cfg`, `CaGcRootLocalPartManifestCore_witness_*.cfg`,
`CaGcRootLocalPartManifestCore_live.cfg`

**What it proves.** The full root-local part-manifest GC protocol (spec
`2026-06-26-cas-gc-streaming-sharded-redesign-design.md` rev.15), including precommit + missing-body
states, owner transitions, orphan sweep, mutable manifests, token-diff discovery (Phase 2), lazy trim
(Phase 3, all-shard fresh fence only), target-sharded reducers (Phase 4), retire-token optimization
(Phase 5), and attempt-scoped generation visibility (Phase 6). Invariants: `INV_NO_DANGLE`,
`INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`, `NoManifestIdReuse`, `RefMatchesBody`,
`ManifestNamespaceMatches`, `SingleManifestOwner`, `CommittedManifestBodyRequired`,
`CommittedNoMissingBlob`, `NoCommittedDangle`, `BlobInDegreeMatchesActiveManifests`,
`FoldedEdgesAreActive`, `ManifestActivationMatchesEdges`; action property `MonotoneGC`. Liveness:
`OrphanManifestDebrisDrains` and `NoLeakForever` under `FairSpec`.

**Positive stages (all HOLD):**

| Stage | Config | Distinct states | Wall |
|---|---|---|---|
| 0 type/journal coverage | `stage0` | 19,846 | 0s |
| 1 identity + body validation | `stage1` | 402,034 | 2s |
| 2 owner transitions + precommit + promote | `stage2` | 68.6M | 7m11s |
| 3 full GC pipeline | `stage3` | 365.6M | 27m45s |
| 4 manifest cleanup + orphan sweep + mutable | `stage4` | 27.4M | 3m29s |
| 5 token-diff discovery | `stage5_tokendiff` | 8.3M | 28s |
| 5 lazy trim (Phase 3) | `stage5_lazytrim` | 338.8M | ~21m |
| 5 target-sharded reducers (Phase 4) | `stage5_sharding` | 983.9M | ~65m |
| 5 retire-token optimization (Phase 5) | `stage5_retiretoken` | 3.5M | <1m |
| liveness | `live` | 17.8M | 30m10s |
| Phase 6 attempt-scoping | `stage6_attemptscoping` | 11.7M | — |

**28 negative controls — all produce their named counterexample (no unexpected pass):**

Selected critical sabotages:

| # | Config | Rule removed | Violated |
|---|---|---|---|
| 3 | `sab_splitpromote` | promote = two CAS with a gap, no fail-closed | `INV_NO_DANGLE` |
| 5 | `sab_commitskipblobreval` | committed publish skips blob revalidation | `INV_NO_DANGLE` |
| 7 | `sab_noorphansweep` | omit pre-precommit debris sweep | `OrphanManifestDebrisDrains` |
| 11 | `sab_deletebodybeforedecrements` | delete body before decrements durable | `NoLeakForever` |
| 12 | `sab_cutoverclaim` | cursor past unsealed deltas | `INV_NO_DANGLE` |
| 14 | `sab_nofence` | skip global fence | `INV_NO_DANGLE` |
| 25 | `sab_lazyfenceunsafe` | reuse stale parent fence position | `INV_NO_DANGLE` (24.5M states) |
| 26 | `sab_reducerownsfence` | target reducer fences only its own shard | `INV_NO_DANGLE` |
| 27 | `sab_crosssharddisplacement` | scatter drops displaced old-binding `-1` deltas | `INV_NO_LOSS` |
| 28 | `sab_staletokenoverdelete` | stale stored token triggers destructive `Land` | `INV_NO_LOSS` |

Full table in `CaGcRootLocalPartManifestCore_RESULTS.md`.

**Key design decisions driven by this model:**

- The fence is always all-shard fresh (reusing a stale parent fence position is load-bearing-unsafe —
  `sab_lazyfenceunsafe` is a permanent negative control for this shape).
- The target-sharded fold uses ONE global coordinator fence (`GCoordFence` over every root shard) —
  independent per-shard fences by a reducer leave another shard's fence stale-low.
- The scatter must emit paired `-1` old-binding deltas; inferring the old target from the new ref alone
  under-counts in-degree for a cross-shard surviving ref.
- The retire-token source (`storedTok`) must never over-delete: the destructive `Land` gate uses the
  STORED token only if it matches the exact current `tokOf` (the stale-disjunct sabotage shows
  `INV_NO_LOSS` not `INV_NO_RETURN` is the violation when a live re-incarnated object is over-deleted).
- Deposed-leader GC attempts must never be reader-visible: only the ADOPTED attempt's artifact can
  be consulted (Phase 6 `INV_ONLY_ADOPTED_VIEWABLE`).

> **Note — fence-era controls document a SUPERSEDED mechanism.** The all-shard fence and its negative
> controls (`sab_nofence` #14, `sab_lazyfenceunsafe` #25, `sab_reducerownsfence` #26) prove the
> *fence-based* create-ordering that the ack-floor redesign (Area 11) replaced with a causal
> acknowledgement floor. They remain valid as evidence that fencing only changed shards or reusing a
> stale fence position was unsound *within that mechanism* — which is exactly why the mechanism, not a
> patch of it, was replaced. This model's section is kept as historical evidence and is **not**
> deleted; the live round protocol and its proofs are `CaGcAckFloorCore.tla` +
> `CaGcAckFloorZombie.tla` (Area 11). The fold, manifest-cleanup, orphan-sweep, source-edge, and
> attempt-scoping results above are unaffected by the redesign and stay CURRENT.
>
> **Audit note (2026-07-22): the `EnableSharding` arm no longer runs — a regression, not just
> supersession.** All three sharding configs — the POSITIVE stage `stage5_sharding` (historically
> 983.9M states, now crashes at 158) and the sabotages `sab_reducerownsfence` (#26) and
> `sab_crosssharddisplacement` (#27) — throw a TLC `RuntimeException` (`CHOOSE m ∈ {}` at `TheM`,
> `.tla:113`, reached from the `GReduceShard`/scatter path): a both-empty edge (`e.old = e.new = {}`)
> reaches the `IF e.old # {} THEN TheM(e.old) ELSE TheM(e.new)` extraction and applies `CHOOSE` to an
> empty set. So the sharded scatter/reduce machinery currently has NO working positive gate in this
> model, and its two fence-era sabotages crash rather than cleanly violate. The sharding correctness
> is still exercised by the C++ `gtest_cas_gc_shard_plan.cpp`; fixing the model's sharding arm (guard
> or repair the both-empty edge in the scatter) is proof-model surgery folded into the deferred
> fence/recheck-and-sharding realignment (see §area-ackfloor), not an unattended edit. The
> non-sharding stages (`stage0..4`, `stage5_tokendiff`/`_lazytrim`/`_retiretoken`,
> `stage6_attemptscoping`, `live`) and their sabotages are unaffected.

**Code currency:** CURRENT for the fold/manifest/attempt-scoping machinery the ack-floor round reuses;
the fence/recheck phases it models are SUPERSEDED by Area 11 (controls kept as historical evidence).
The largest and most comprehensive model in the corpus.

### `SkipParksDeadPrecommit` — dangling-precommit orphan gate (C++ fix LANDED) {#skipparksdeadprecommit}

**Files:** `CaGcRootLocalPartManifestCore.tla` + `CaGcRootLocalPartManifestCore_sab_skipparksdeadprecommit.cfg`
(bug) / `_fix_skipparksdeadprecommit.cfg` (fix).

**What it proves.** The `DANGLING-PRECOMMIT` manifest orphan found in the `utils/ca-soak` S30 churn
scenario (2026-07-07): an abandoned precommit (activated body, never promoted, never removed) sits on a
content-static ref-shard; the token-diff `Skip` parks the shard, so the fold-visit
`reclaimAbandonedPrecommit` never re-runs, and the precommit — provably dead once the watermark advances
past its `build_sequence` — orphans its manifest forever (the orphan sweep spares it because
`activeManifestKeys` keeps the un-removed precommit binding live). The liveness property
`LiveDeadPrecommitReclaimed` (a present, body-present, still-bound, watermark-dead abandoned precommit is
eventually reclaimed, under `WF` of `GReclaimDeadPrecommit`) is **VIOLATED** with
`SabotageSkipParksDeadPrecommit = TRUE` (the shipped bug — the static shard is parked forever) and
**HOLDS** with `= FALSE` (the fix: `CanSkipShard` force-Reads a shard holding a watermark-dead live
precommit, keyed on the same durable death fact `reclaimAbandonedPrecommit` uses). TLC v2.19: bug cfg
violated (174,024 distinct states); fix cfg no error (800,072 states). `GReclaimDeadPrecommit` refines
`WAbandonPrecommit` (introduces no new reachable state), so the fix only restricts WHEN reclaim fires.
All four `EnableTokenDiff = TRUE` sibling cfgs re-verified with the added constant — no counterexample
masked; `sab_skipchangedshard` still violates `INV_NO_DANGLE`.

**C++ fix (LANDED, branch `cas-gc-rebuild`).** `Gc::computeDiscoverDecisions` overrides a would-be `Skip`
to Read when the sealed `ShardCoverage`'s minimal live precommit `isPrecommitDead` vs the namespace mount
watermark (`ProfileEvents::CasGcPrecommitRevisitForced`); `Gc::fold` stamps the minimal live precommit
into the coverage; the shared `isPrecommitDead` helper is used by both the guard and
`reclaimAbandonedPrecommit`. Unit test `CasDanglingPrecommit.*` (deterministic: reclaim after the
watermark advances; idempotency; Skip preserved for live/no precommit; double-removal idempotency). S30
regression: pre-fix left 1 `_manifests` orphan (FAIL); post-fix ×4 seeds all PASS with residual 0
(`reclaimAbandonedPrecommit` observed firing live in one seed via the normal Read path — the specific
force-Read/parked-static-shard timing is a rare race exercised deterministically only by the unit test).

**Follow-up (out of scope, backlogged):** the write-path `PROMOTE-OVER-COMMITTED-LEAK` (a distinct
writer-side leak reachable via a `republishRef` crash re-drive) and `ABANDON-RETIRE-ORDERING`.

**Code currency:** CURRENT (the C++ fix matches this gate).

### `AtMostOneCommittedManifestPerRef` — promote-over-committed gate (C++ fix LANDED) {#atmostonecommittedmanifestperref}

**Files:** `CaGcRootLocalPartManifestCore.tla` + `CaGcRootLocalPartManifestCore_stage2.cfg`.

**What it proves.** The `PROMOTE-OVER-COMMITTED-LEAK` write-path bug (2026-07-08): `Build::promote`
overwrote `refs[ref]` with a Δ=0 owner-move without releasing a pre-existing *different* committed manifest
`T_old`, leaving two committed bindings for one ref and orphaning `T_old` (owner↔refs divergence);
reachable via a `republishRef` crash re-drive (RENAME/DETACH-ATTACH). The model's `WPromote`/
`WPublishCommitted` ALREADY enforce `RefFreeFor(ref, m)` (a ref owns ≤1 committed manifest) — the shipped
C++ simply diverged. The new invariant `AtMostOneCommittedManifestPerRef ==
\A r \in Refs : Cardinality({m \in ManifestIds : owner[m] = r}) <= 1` makes that property TLC-checked; it
**HOLDS** in `stage2` (68.5M distinct states) under the enforced `RefFreeFor`. It is logically equivalent
to the pre-existing `SingleManifestOwner` (same ref→manifest direction) — a dedicated, named regression
gate rather than new coverage. The bug itself is reproduced deterministically by the C++ RED test
(`CasPromoteRepublish.*`), so no `SabotagePromoteOverwritesCommitted` negative control was added (it would
force editing all 47 sibling cfgs for marginal added assurance).

**C++ fix (LANDED, branch `cas-gc-rebuild`).** `Build::promote` fail-closes with `ABORTED` when
`refs[final_ref_name]` already names a different committed `manifest_ref` (a same-manifest re-promote and an
absent ref proceed); `ContentAddressedTransaction::republishRef` is idempotent on the destination (skip the
publish + `dropRef(src)` when dst is already committed with the same path-sorted `entries`; `ABORTED` on a
different-content conflict), so RENAME/DETACH-ATTACH re-drives no longer leak; and `Build::abandon` retires
its build_seq only after the precommit-removal CAS (`ABANDON-RETIRE-ORDERING`, closing the double-removal
window that the just-landed dangling-precommit fix made more frequent). Unit tests: `CasPromoteRepublish.*`.

**Code currency:** CURRENT (the C++ fix matches this gate).

---

## Area 8 — In-degree re-fold undercount (B-indeg fix) {#area-indeg-refold}

### `CaGcIndegRefoldCore.tla` — in-degree re-fold undercount {#cagcindegrefoldcore}

**Files:** `CaGcIndegRefoldCore.tla`, `CaGcIndegRefoldCore_fix.cfg`, `CaGcIndegRefoldCore_sab.cfg` —
**removed 2026-07-22** (models a superseded integer-accumulation design; full text in git history).

> **Removed — the shipped in-degree design is no longer a non-idempotent integer stream.** The
> `cas-gc-rebuild` fold computes in-degree by an **idempotent two-cursor presence-set merge** keyed
> by `(blob_hash, source_id)` (`CasBlobInDegree.cpp:380-389`: "prior present + activate ⇒ present;
> any remove ⇒ absent"); the surviving-edge count is a `uint64_t` that structurally cannot go
> negative, and there is no `merged < 0 → CORRUPTED_DATA` guard. This model existed precisely to
> catch the integer-underflow that the idempotent edge-set recompute cannot express — but the code
> adopted exactly that idempotent recompute, so the hazard is now structurally impossible and the
> model describes a design the code abandoned (its three code citations had all drifted). Not a
> CODE-RISK: the property it guarded holds by construction. Retired for the same reason as the EBR
> `CaGcCore.tla` — a superseded-design model.

**What it proved.** The minimal model of the bug H1b: the completion-seal cursor must be persisted at
`max(foldCursor, fenceVersion)` (past what recheck already folded), not at `foldCursor` (the
pre-window fold-time cursor). If the seal persists `foldCursor`, the next round's fold reconstructs its
parent cursor from that seal and re-folds the fence-window removal, driving the integer in-degree
counter to `-1` → `INV_INDEG_NONNEGATIVE` violated (in the C++, `merged < 0` → `CORRUPTED_DATA`).

Invariant: `INV_INDEG_NONNEGATIVE` (`∀ b : indeg[b] >= 0`). Bounds: `Blobs={b1}`, `MaxLog=4`,
`MaxRound=3`.

| Config | `SabotageCompletionCursorAtFold` | Result |
|---|---|---|
| `_fix` | FALSE (fix: cursor = `max(foldCursor, fenceVersion)`) | PASS |
| `_sab` | TRUE (bug: cursor = `foldCursor`) | `INV_INDEG_NONNEGATIVE` VIOLATED |

**Why this model is necessary (not caught by `CaGcRootLocalPartManifestCore`):** the large model
recomputes in-degree from a folded EDGE SET — set-difference recompute is idempotent, so re-folding is
a no-op there. The C++ accumulates in-degree as a NON-idempotent INTEGER delta stream; re-folding an
already-absorbed removal drives the counter negative. This minimal model targets that concrete
implementation class directly.

**Design decision:** the `Seal` action must advance `persistedCursor` to `fenceVersion` (not remain at
`foldCursor`) so the next round's `parentCursor` starts past the events recheck already consumed.

**Code currency:** REMOVED 2026-07-22 — superseded integer-accumulation design (see the note above).

---

## Area 9 — Shard incarnation and registry removal (D1) {#area-shard-incarnation}

### `CaGcShardIncarnationCore.tla` — registry removal gate (D1 Phase 0) {#cagcshardincarnationcore}

**Files:** `CaGcShardIncarnationCore.tla`, `CaGcShardIncarnationCore_design.cfg`,
`CaGcShardIncarnationCore_sab_newbornnofloor.cfg`, `CaGcShardIncarnationCore_sab_pathkeyedcursor.cfg`,
`CaGcShardIncarnationCore_sab_deletebeforefold.cfg`, `CaGcShardIncarnationCore_sab_incarnationreuse.cfg`

**What it proves.** With the namespace registry removed (proven load-bearing in `CaIncarnationCore` via
`sab_noregistry`), two replacement coordinates keep the safety invariants: (1) a durable never-reused
per-`(ns,shard)` incarnation (`sInc`, from a per-shard high-water `sIncMax`), and (2) a newborn shard
born fenced to the current `gcRound` (self-floor). Invariants: `INV_NO_DANGLING` (no committed ref to
an absent/dead-token blob) and `INV_NO_ORPHAN_EDGE` (no folded edge outlives its shard object).

| Config | Flags | Result | States |
|---|---|---|---|
| `_design` | none | ✅ No error | 5,872,030 |
| `_sab_newbornnofloor` | drop round self-floor, keep incarnation | ❌ `INV_NO_DANGLING` violated | — |
| `_sab_pathkeyedcursor` | drop incarnation from cursor, keep round | ❌ `INV_NO_DANGLING` violated (ABA) | — |
| `_sab_deletebeforefold` | delete before journal fully folded | ❌ `INV_NO_ORPHAN_EDGE` violated | — |
| `_sab_incarnationreuse` | recreate draws ≤ `sIncMax` (same-path ABA) | ❌ `INV_NO_DANGLING` violated | — |

**Design decisions driven by this model:**

- Neither coordinate alone suffices: the pool-global round closes the publish-race the registry
  previously closed; the per-shard incarnation prevents ABA confusion of a delete+recreate at the same
  path. The design's two-coordinate model is minimal.
- Per-shard monotonicity is the invariant, not global uniqueness. Cross-shard incarnation collision is
  safe (the fold cursor keys by `(shard, incarnation)`, so the same value in two shards is not ABA);
  same-shard reuse across a delete+recreate IS dangerous.
- Reclaim ordering is load-bearing: delete must wait until the journal is fully folded (tombstone
  included).
- The registry can be deleted — the spec's `pending-newborns` ephemeral fallback is NOT needed.

**Code currency:** CURRENT (D1 Phase 0 gate, 2026-07-01).

---

## Area 11 — Ack-floor GC fence redesign {#area-ackfloor}

These two focused models prove the one-pass ack-floor round that replaced the per-round all-shard
fence and the fold-through-fence recheck (spec `2026-07-02-cas-gc-ack-floor-fence-redesign.md`; GC
protocol `04 §gc-round`). They are the formal gate for that redesign; the fold/manifest machinery
the round reuses stays proved by `CaGcRootLocalPartManifestCore.tla` (Area 7).

> **Note — the writer-heartbeat half is superseded by v3 freshness-meta (2026-07-10), the GC-side round
> half is not.** Both models foreground writer `observed_gc_round` heartbeats and a `min_ack` floor
> (`condemn_round < min_ack`) as the graduation gate. The freshness-meta redesign
> (`docs/superpowers/plans/2026-07-10-cas-freshness-meta-v3.md`) removed exactly that half:
> `CasRetireView.{h,cpp}` (the writer-side retired-view download this ack floor fed) is deleted;
> `Core/Proto/cas_format.proto` reserves the wire field (`reserved 10; // was observed_gc_round; the
> writer-side retired-view ack floor was removed (v3 Task 6)`); and `Gc::graduationDue` (`CasGc.cpp`)
> now paces graduation on GC rounds alone, per its own comment — "graduation itself paces on GC rounds
> via `new_round`, not on heartbeat acks". The GC-side round pipeline these models also cover —
> `GBegin`/`GFold`/`GComplete`, the two-step commit gate, `graduationDue`'s `condemn_round < current_round`
> check, disaster-recovery `GRebuild`, the clamp-suppression guard (§area-clamp-suppression) — is
> UNCHANGED and stays CURRENT. Only the writer-heartbeat/`min_ack` half of what these models prove is
> historical; the models are kept as the record of the mechanism they gated, not deleted.

### `CaGcAckFloorCore.tla` — ack-floor round core {#ackfloor-core}

**Files:** `CaGcAckFloorCore.tla`, `CaGcAckFloorCore_stage1.cfg`, `CaGcAckFloorCore_sab_*.cfg`,
`CaGcAckFloorCore_witness_*.cfg`. Run wrapper: `run_ackfloor.sh`.

**What it proves.** Writers advertise `observed_gc_round` through heartbeats; GC graduates a retired
entry only when `condemn_round < min_ack` computed over heartbeats that are **live OR
expired-but-not-fenced**; a fenced heartbeat can never renew or land a commit again. Commits are
two-step (`WPrepare` = gate evaluation, `WLand` = CAS response) so the in-flight window is a real
interleaving, and the pass is three steps (`GBegin` / `GFold` / `GComplete`) so a commit landing
between the fold cut and the deletes is a real interleaving. The model also includes disaster-recovery
rebuild (`GRebuild`, spec `2026-07-03-cas-gc-rebuild-design.md`, with a `lostRefs` ghost variable for a
sabotaged rebuild's dropped edges), sourceless copy-forward (`WCopyForward`), and an honest fold clamp
(`clampedL` / `clampedEver`) with a suppression guard in `GComplete`. Invariants: `INV_NO_DANGLE` (no
landed, folded, or rebuild-lost ref points at an absent blob), `INV_NO_RETURN` (no ref binds a deleted
incarnation), `INV_ACK_LE_VIEW` (the honest ack never runs ahead of the installed view).

Bounds: `Writers = {w1, w2}`, `Blobs = {b1}`, `MaxRound = 4`, `MaxTok = 4`.

**Eleven sabotages (negative controls — each breaks exactly one load-bearing rule and MUST produce a
counterexample):**

| Config | Rule broken |
|---|---|
| `sab_ignorefloor` | graduate ignoring the floor entirely (`condemn_round ≥ min_ack`) |
| `sab_ackwithoutread` | ack advances without installing the retired view |
| `sab_ackbeforedrain` | ack advances while an old-view commit is still in flight (no drain) |
| `sab_sleeperrearm` | the floor excludes expired-UNFENCED heartbeats (assumes dead without a fence-out) |
| `sab_skipshard` | the fold cut leaves one landed ref unconsumed |
| `sab_adopttoken` | the commit gate references a visibly-retired token instead of recreating |
| `sab_openbeforeload` | a fresh mount starts with an unloaded (round-0) view and mutates before loading |
| `sab_rebuilddropedge` | rebuild loses one committed owner's folded ref |
| `sab_rebuildkeepretired` | rebuild carries the old retired entries into the new baseline |
| `sab_rebuildlowround` | rebuild mints a round below surviving mount acks |
| `sab_clampnosuppress` | graduation ignores a declared clamp — reproduces the 2026-07-03 night incident |

**Six witnesses (negated reachability — a TLC "violation" means the state IS reachable):**
`W_DeleteHappens` (condemn → graduate → delete), `W_SpareHappens` (condemn → recover → spare),
`W_RecreateHappens` (recreate → `TokenMismatch` on the pending delete), `W_CopyForwardHappens`
(sourceless copy-forward taken), `W_RebuildHappens` (raw rebuild taken), `W_ClampHappens` (an honest
clamped pass occurred).

**Design decisions driven by this model:**
- Graduation is gated purely by the causal floor (`condemn_round < min_ack`) — no wall clock on the
  happy path.
- The ack must not advance without loading the retired list, and not before the drain completes.
- An expired heartbeat must be fenced-out (not merely assumed dead) before it drops out of the floor.
- The commit gate must recreate a listed token, never adopt it.
- Rebuild must discard the old retired list and mint a round above every surviving mount ack (see
  §area-clamp-suppression for the clamp/rebuild/copy-forward extension detail).

**Code currency:** MIXED — the graduation gate this model checks is SUPERSEDED; the rebuild and
clamp-suppression parts still MATCH (audited 2026-07-22, deeper than the "minor drift" this note used
to claim). The model's `GComplete` graduation predicate is `e.r < minAckL`, a **writer-ack floor**
(`MinAck` over `wAck` of the live/expired `FloorSet`) — but the shipped `Gc::graduationDue` paces on
`condemn_round < current_round` alone, with **no** writer-ack input (`CasGc.cpp:1867`; the merge gate
is `!suppress_destructive && e.condemn_round < current_round`, `CasBlobInDegree.cpp:431`). So the
model's entire writer-heartbeat apparatus — `WOpen`/`WBeat` advertising a round into `wAck`,
`FloorSet`/`MinAck`/`minAckL`, and the `sab_sleeperrearm` / `sab_ackbeforedrain` / `sab_ackwithoutread`
sabotages and `INV_ACK_LE_VIEW` that test it — models a graduation input the code no longer has. What
still MATCHES and gives the model its unique current value: clamp suppression (`~clampedL` guarding
graduation ↔ `suppress_destructive`, `CasGc.cpp:1360`), disaster-recovery `GRebuild` (↔
`Gc::rebuildBaseline`, `CasGc.cpp:1909`: empty retired list, over-protect unowned-alive, round minted
above surviving numbers), the three-step cut + two-step commit window, and exact-token delete. Not a
CODE-RISK: round-pacing is a stricter safety condition than the ack floor, and the current graduation
is independently proven by `CaGcRoundDeferCore` + `CaGcCondemnMarkerGate` + the two-phase
`CaGcAckFloorZombie`. To realign, trim the ack apparatus and re-base graduation on the latched round,
keeping the `GFenceOut`/`WExpire` fence-out only as *liveness*; this is a scoped proof-model rewrite
(false-green risk — kept for a careful pass with adversarial review), not done here.

---

### `CaGcAckFloorZombie.tla` — two-leader `delete_pending` gate {#ackfloor-zombie}

**Files:** `CaGcAckFloorZombie.tla`, `CaGcAckFloorZombie_stage1.cfg`,
`CaGcAckFloorZombie_sab_eagerdelete.cfg`, `CaGcAckFloorZombie_witness_delete.cfg`. Run wrapper:
`run_ackfloor_zombie.sh`.

**What it proves.** The two-phase graduation (`delete_pending`, Task-9 amendment; `04
§two-phase-graduation`) is load-bearing when **two** leaders' passes fully interleave. Each leader
latches `(round, retired list, fold-cut in-degrees)` at `GBegin`, deletes pre-publish, and its
publish CAS succeeds only if `(round, retired)` are unchanged since the latch (the `gc/state` token
guard) — so a deposed leader's merge output evaporates but its deletes do not. Restricting pre-CAS
deletes to entries **already published pending by a previous round's CAS** keeps a deposed leader's
arbitrarily-stale snapshot from deleting a live blob.

Bounds: `Writers = {w1, w2}`, `Leaders = {l1, l2}`, `Blobs = {b1}`, `MaxRound = 5`, `MaxTok = 4`.
Honest stage (`SabotageEagerZombieDelete = FALSE`) is clean (~2 M distinct states) on `INV_NO_DANGLE`
+ `INV_NO_RETURN`. `sab_eagerdelete` (a pass ALSO deletes its FRESH graduations — the pre-amendment
single-phase behavior) yields the `INV_NO_DANGLE` counterexample, proving `delete_pending`
load-bearing. `witness_delete` shows the pending-delete path is reachable.

**Order invariant surfaced by this model (the implementation must never lose it):** the ack floor is
latched **no later than the fold cut**. A floor read *after* the cut would see acks advertised by
writers whose in-flight commits landed after the cut (invisible to this pass's in-degrees), and a
fresh graduation could then go pending over a live reference. Pinned as a comment at the
`computeHeartbeatFloor` call site and in the spec's TLA+ section.

**Code currency:** CURRENT (minor drift; partially superseded — v3 freshness-meta, see the note above).
The two-phase `delete_pending` graduation and the deposed-leader `gc/state` CAS guard are GC-side
mechanics and stay CURRENT (`cas-gc-ack-floor-fence`); graduation no longer depends on a writer
heartbeat ack.

---

## Area 10 — Superseded EBR/epoch GC core {#area-ebr}

### `CaGcCore.tla` — EBR epoch/generation design (superseded) {#cagccore}

**Files:** `CaGcCore.tla`, `CaGcCore_stage*.cfg` — **removed 2026-07-21** (superseded design; this
section stays as the historical record, full text in git history).

The original GC core, based on Epoch-Based Reclamation (EBR) with a monotone `epoch_current` counter,
per-writer `O_W` pins, and a `+`/`-` event fold. Checked at stages 1–4 (full adversarial: expiry gap,
split-brain, total Keeper wipe); all four stages PASS (`INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_ABA`,
`TypeOK`). **Superseded** by `CaIncarnationCore.tla` (incarnation-token design, 2026-06-10+). Four
counterexamples found during EBR model development encoded the four load-bearing rules:

- **CE-1** (`INV_NO_LOSS`): flush-`+`-then-advance must cover the decide→`+`-durable window. A writer
  advancing `O_W` before its `+` is durable allows GC to condemn and delete the epoch while the writer
  still holds an unpublished ref.
- **CE-2** (`INV_NO_LOSS`): the reuse decision must target an epoch the writer currently observes
  (`e ≥ O_W[w]`); reusing an epoch the writer already advanced past means its live lease no longer
  covers the dependency.
- **CE-3** (modeling artifact): a shared `plusDurable` boolean collapsed all `+(e)` into one flag;
  a drop removed another writer's in-flight reuse pin. Fixed by per-writer `pin` sets.
- **CE-4** (real design constraint, `INV_NO_LOSS`): failing on `Disconnected` alone does not close the
  `[t_expire, t_aware]` session-expiry gap. A self-fence on a local elapsed-time deadline strictly
  inside `T_session` is required.

**Code currency:** SUPERSEDED. Replaced by the incarnation-token design; files removed 2026-07-21.

---

## Summary table {#summary-table}

| Model | Area | Status | Key invariant(s) | Sabotages | Design decisions |
|---|---|---|---|---|---|
| `CaIncarnationCore.tla` | GC core | **CURRENT as safety spine**; concrete structure superseded (manifest-embedded journal/fence vs ref-log/source-edge-run) | `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE` | 11 | fence, recheck, exact-token delete, atomic cascade, registry fence-time universe, evidence re-observation |
| `CaIncarnationProofCore.tla` | GC core (Apalache) | **REMOVED 2026-07-22** (stale pre-B91; unverifiable — no Apalache) | `IndInv` (19 conjuncts) inductive at fixed bounds | 5 negative controls | `W-REVALIDATE` is load-bearing (F1 machine-checked); `InflightCurrentUnreferenced` is irredundant |
| `CaBuildRootPrecommit.tla` | Precommit/B140/B199-S2 | **CURRENT conclusion**; 2 mechanisms drifted (inline-closure → lazy-fold+clamp-barrier; presence-gate → owner-liveness) | `INV_NO_DANGLE_COMMITTED`, `INV_BUILDROOT_PROTECTS`, `INV_COMMIT_FAILCLOSED`, `INV_NO_LEAK` | 2 + 1 liveness | build-root + fail-closed commit jointly necessary; inline closure at precommit time |
| `CaGcLeaseCore.tla` | Lease/B160 | **CURRENT** | `NoEpochCollision`, `NoFalseSteal` | 1 | advisory heartbeat eliminates false steals; safety independent of heartbeat |
| `CaCasMountCore.tla` | Mount | **CURRENT** (rev.6 observation-reclaim; audited 2026-07-22) | `NoTwoServerUuids…`, `ForeignUuid…`, `WriterEpochMonotoneUnique`, `GlobalSupersededWriter…` | 5 (incl. `sab_wallclockreclaim`) | sticky owner, monotone epoch, observation-based reclaim on own clock (not foreign wall clock), pure-local per-write guard |
| `CaB140DangleMerge.tla` | B140 fix proof | **CURRENT** (history record) | `INV_NO_LOSS` | 2×2 matrix | trim-gate + cursor-in-snap jointly necessary |
| `CaB140DangleFaithful.tla` | B140 history | **REMOVED 2026-07-22** (dead-mechanism refutation) | `INV_NO_LOSS` | — | Phase-1 mechanism clean with faithful producers |
| `CaB140Dangle.tla` | B140 history | **REMOVED 2026-07-21** (unfaithful producers) | — | — | Phase-1 investigation record |
| `CaResurrectLiveness.tla` | Resurrect/B167 | **REMOVED 2026-07-21** (stale, deferred M-F guard) | `<>published` | 1 | upload→publish span not atomic; heartbeat guard load-bearing |
| `CaBuildWatermark.tla` | Watermark/B167 | **REMOVED 2026-07-21** (blob-guard removed by B171) | `Inv_ProtectedNeverCondemned`, `Inv_NoDangle`, liveness | 3 | monotone `build_seq`, exact min active set, sound crash detection |
| `CaBuildWatermarkNum.tla` | Watermark numeric | **REMOVED 2026-07-21** (blob-guard removed by B171) | `Inv_ProtectedNeverCondemned`, `Inv_NoDangle` | 2 | monotone `build_seq` (not just unique), per-server scoping |
| `CaGcRootLocalPartManifestCore.tla` | Part-manifest GC R0 | **CURRENT** (fold/manifest/attempt-scoping); fence/recheck phases SUPERSEDED by Area 11 | `INV_NO_DANGLE/LOSS/RETURN`, 10 more; liveness | 28 | all-shard fresh fence (superseded), single coordinator fence (superseded), scatter deltas, stale-token-no-over-delete, attempt-scoped visibility |
| `CaGcIndegRefoldCore.tla` | Indeg re-fold | **REMOVED 2026-07-22** (superseded integer-delta design) | `INV_INDEG_NONNEG` | 1 | seal cursor at `max(foldCursor, fenceVersion)`, not `foldCursor` |
| `CaGcShardIncarnationCore.tla` | Registry removal D1 | **CURRENT** | `INV_NO_DANGLING`, `INV_NO_ORPHAN_EDGE` | 4 | two-coordinate replacement (incarnation + round self-floor) for registry; per-shard monotonicity |
| `CaGcAckFloorCore.tla` | Ack-floor round core | **MIXED**: graduation gate (writer-ack floor) SUPERSEDED by round-only pacing; `GRebuild` + clamp-suppression still MATCH | `INV_NO_DANGLE`, `INV_NO_RETURN`, `INV_ACK_LE_VIEW` | 11 | causal floor gates graduation; ack after drain + view load; expired ⇒ fence-out; recreate not adopt; rebuild discards retired list + mints round above all acks; clamp suppression gates graduation |
| `CaGcAckFloorZombie.tla` | Ack-floor two-leader | **CURRENT** (minor drift; writer-heartbeat half superseded — v3) | `INV_NO_DANGLE`, `INV_NO_RETURN` | 1 | `delete_pending` two-phase graduation load-bearing; floor latched ≤ fold cut (order invariant) |
| `CaGcCore.tla` | EBR GC core | **REMOVED 2026-07-21** (superseded) | `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_ABA` | 4 CEs during dev | EBR design record; replaced by incarnation-token |
| `CaGcRoundDeferCore.tla` | GC round DEFER/skip-unchanged | **CURRENT** | `NoOverDelete`, `NoDangle`; `EventuallyFolded` | 2 | a due graduation force-folds first (no destructive decision on a not-fully-folded snapshot); deferral bounded (`deferCount < MaxDefer`) |
| `CaEdgeBeforeObserve.tla` | Writer/GC simplification Gate A | **CURRENT** for no-tokened-reval + order + K1; **K3Head/K3AdoptCheck DRIFTED** (tokenless leaf now manifest-trusted, no promote probe) | `NoOverDelete`-shaped safety (implicit) | 4 | with EDGE-BEFORE-OBSERVE + same-pass decided-delete, promote-time revalidation of TOKENED leaves is redundant; K1/K3Head/K3AdoptCheck + the order itself stay load-bearing |
| `CaMetaDescriptor.tla` | Writer/GC simplification Gate B (meta descriptor, v1) | **REMOVED 2026-07-22** (`INV-META-BODY` linearizer framing false vs shipped advisory meta) | `INV-META-BODY` | 7 | create bottom-up (body, then meta); delete top-down (meta at captured etag, then body at condemn-time token) |
| `CaMetaDescriptorRaw.tla` | Gate B raw-body / terminal-tombstone | **REMOVED 2026-07-21** (rejected by v3) | `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_META_BODY` | 5 | raw immutable bodies force a terminal tombstone + writer-waits-on-GC coupling; rejected in favor of keeping the in-body incarnation tag |
| `CaMetaIncarnationKey.tla` | Gate B Option B (per-incarnation body keys) | **REMOVED 2026-07-21** (rejected) | `INV_NO_DANGLE` (implicit) | 1 | removes the tombstone/wait but reintroduces the already-rejected generation-in-key design (404→LIST, manifest carries incarnation) |
| `CaManifestSweepWindow.tla` | Orphan-sweep vs removal-fold wedge | **REMOVED 2026-07-22** (gtest covers) | `INV_FOLD_PROGRESS` | 1 | the orphan sweep must skip a committed body with a pending (unsealed) removal — the removal-fold still needs the body to emit its decrement |

---

## Running the models {#running-models}

All models run from `docs/superpowers/models/`. TLC jar expected at `../../../tmp/tla2tools.jar`
(v2.19). Apalache binary at `../../../tmp/apalache/bin/apalache-mc` (v0.58.0+).

Runners come in two shapes. **Suite runners** take no arguments, own their model's whole config list
and assert each expected outcome *by the name of the invariant or property it must break*:
`run_refsnaplog.sh`, `run_deltaintake.sh`, `run_refcatalog.sh`, `run_nscleanup_staleleader.sh`,
`run_mount.sh`, `run_buildrootprecommit.sh`, `run_disklifecycle.sh`, `run_erasureproof.sh`,
`run_foldclamp.sh`, `run_refwcleanup.sh`. The rest (`run_tlc.sh`, `run_gc_partmanifest.sh`,
`run_ackfloor.sh`, …) are single-config drivers: they take a cfg as `$1`, run it, and assert nothing
— the loops below supply the expectation, so read their comments before trusting a colour.

```bash
# incarnation core — main staged suite
./run_tlc.sh CaIncarnationCore_stage1.cfg
# ... stages 2, 3, 4_small, 4_journaltree, 5_small, 6_registry, 6_evstale
# Sabotages: each MUST violate. Assert a NAMED violation, never a bare nonzero exit — TLC also exits
# nonzero on a parse error, an unspecified successor state and a deadlock report, which is exactly how
# a broken model masquerades as a working sabotage (2026-07-28: that is what hid a live parse bug in
# CaBuildRootPrecommit; see models/2026-07-28-v9-phase-RESULTS.md {#fix-runners}). This prints the
# invariant each sabotage actually broke — compare it against the entry for that config above.
for c in nofence norecheckfold noretireview unconddelete reusedtag cascade \
         cutoverclaim noreobserve noregistry foldtimeuniverse noevreobserve; do
  ./run_tlc.sh CaIncarnationCore_sab_$c.cfg >/dev/null
  broke=$(grep -oE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' \
            ../../../tmp/tlc_CaIncarnationCore_sab_$c.log | head -1 \
          | sed -E 's/.* ([A-Za-z0-9_]+) is violated/\1/')
  [[ -n "$broke" ]] && echo "RED  $c -> $broke" \
                    || echo "UNEXPECTED: $c reported no named violation (check the log, not the exit code)"
done

# build-root / precommit — whole suite, each expectation asserted by the name of the invariant or
# property it must break (2026-07-28: replaced a hand-run loop that checked no colours at all, and
# silently omitted the b2 witness)
./run_buildrootprecommit.sh

# part-manifest GC
./run_gc_partmanifest.sh stage3
./run_gc_partmanifest.sh stage4
./run_gc_partmanifest.sh live
# ... all stage5_* and sab_* configs

# shard incarnation / registry removal
java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers auto \
  -config CaGcShardIncarnationCore_design.cfg CaGcShardIncarnationCore.tla

# ack-floor GC round core — positive stage + witnesses
./run_ackfloor.sh CaGcAckFloorCore_stage1
for w in delete spare recreate copyforward rebuild clamp; do ./run_ackfloor.sh CaGcAckFloorCore_witness_$w; done  # each MUST report reachable
# sabotages (each MUST violate) — same named-violation assertion as the incarnation sweep above:
for s in ignorefloor ackwithoutread ackbeforedrain sleeperrearm skipshard adopttoken openbeforeload \
         rebuilddropedge rebuildkeepretired rebuildlowround clampnosuppress; do
  ./run_ackfloor.sh CaGcAckFloorCore_sab_$s >/dev/null
  broke=$(grep -oE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' \
            ../../../tmp/tlc_CaGcAckFloorCore_sab_$s.log | head -1 \
          | sed -E 's/.* ([A-Za-z0-9_]+) is violated/\1/')
  [[ -n "$broke" ]] && echo "RED  $s -> $broke" \
                    || echo "UNEXPECTED: $s reported no named violation (check the log, not the exit code)"
done

# ack-floor two-leader delete_pending gate
./run_ackfloor_zombie.sh CaGcAckFloorZombie_stage1                     # clean
./run_ackfloor_zombie.sh CaGcAckFloorZombie_sab_eagerdelete            # MUST violate INV_NO_DANGLE
./run_ackfloor_zombie.sh CaGcAckFloorZombie_witness_delete             # pending-delete reachable

# in-degree re-fold
java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers auto \
  -config CaGcIndegRefoldCore_fix.cfg CaGcIndegRefoldCore.tla
java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers auto \
  -config CaGcIndegRefoldCore_sab.cfg CaGcIndegRefoldCore.tla  # MUST violate INV_INDEG_NONNEGATIVE

# Apalache inductive invariant (proof core)
# (CaIncarnationProofCore.tla + Apalache.tla + run_apalache.sh were removed 2026-07-22 — stale
#  pre-B91 fragment, unverifiable without an Apalache binary. To revive: install Apalache and
#  re-derive IndInv against the current CaIncarnationCore.tla.)
```

## Clamp + destruction-suppression extension (2026-07-03) {#area-clamp-suppression}

`CaGcAckFloorCore` models the fold-clamp mechanism honestly: `GFold` may hold back one landed ref
— the abstraction of a per-shard cursor frozen at an unreadable manifest body (a false 404, a
bodiless precommit) — but DECLARES it (`clampedL`), unlike `SabotageSkipChangedShard`, which holds
one back silently and remains the lethal lying-fold counterexample. `GComplete`'s graduation is
guarded by `~clampedL` (the implementation's clamp-suppressed pass: no graduations, no pending
deletes while any shard is clamped); held refs stay in `landed`, so a later clamp-free pass
consumes them — the clamp release needs no extra machinery. `SabotageClampNoSuppress` removes only
the guard and reproduces the 2026-07-03 night incident (31 dangling blobs) as an `INV_NO_DANGLE`
counterexample; `W_ClampHappens` witnesses a reachable honest clamped pass. Honest stage-1 is
clean at 83.9M distinct states (the held ref persisting across passes multiplies configurations;
the hold is bounded to one ref — any dangle of this class needs a single held `+1`, and the
suppression rule reads only the boolean declaration).

The same revision adds disaster-recovery rebuild (`GRebuild`, spec
`2026-07-03-cas-gc-rebuild-design.md`) and sourceless copy-forward (`WCopyForward`) to the model.
`GRebuild` recomputes the baseline from owner state at idle: the retired list restarts empty
(over-protect — everything re-condemns through the normal pipeline) and the round is minted above
every mount's advertised ack, so no stale ack can float a fresh condemnation past the floor before
its writer re-observes. A `lostRefs` ghost set records any owner ref a sabotaged rebuild failed to
re-emit (`sab_rebuilddropedge`); `INV_NO_DANGLE` is checked over `landed ∪ folded ∪ lostRefs` so a
dropped edge is caught even though the rebuild itself doesn't retain it. `sab_rebuildkeepretired`
and `sab_rebuildlowround` are the other two rebuild sabotages; `W_RebuildHappens` and
`W_CopyForwardHappens` are the corresponding reachability witnesses.

---

## Area 12 — Resurrect re-upload orphan (2026-07-07) {#area-resurrect-reupload-orphan}

### `CaGcResurrectReuploadOrphan.tla` — resurrect-replaced incarnation orphan {#cagcresurrectreuploadorphan}

**Files:** `CaGcResurrectReuploadOrphan.tla`, `CaGcResurrectReuploadOrphan_bug.cfg`,
`CaGcResurrectReuploadOrphan_fix.cfg` — **removed 2026-07-22** (covered by the deterministic
`CasGcLeak.ResurrectReplaced*` gtests, see the currency note; full text in git history).

**What it proves.** A focused reproduction of the `RESURRECT-REUPLOAD-ORPHAN` leak found in the
ca-soak S30 create/drop churn scenario (root-caused via `system.content_addressed_log`; see
`utils/ca-soak/scenarios/BACKLOG.md`). The liveness property `NoLeakForever` — a present, ever-edged,
unreferenced incarnation is eventually deleted or referenced again, under weak fairness of the GC
fold/delete — is **VIOLATED** with `FixReCondemnCurrentToken = FALSE` (shipped behavior) and **HOLDS**
with `TRUE` (the fix). TLC v2.19, 194 distinct states; the bug counterexample is a lasso: a condemned
token is replaced by a resurrect re-upload (new token at the same content-hash key), the old token's
exact-token delete finds the newer token and skips (`replaced`), and the fold — being touch-gated for
fresh condemn and hash-keyed for settling the prior entry — never re-condemns the replaced token, which
then stutters present forever.

**Why the canonical model missed it (model-vs-code faithfulness gap).** `CaIncarnationCore`'s `GRetire`
condemns by **(hash, CURRENT token)** — guard `~\E e \in retired : e.h = h /\ e.t = tokOf[h]` — so after
a resurrect changes the token it re-condemns the new incarnation, and `NoLeakForever` (checked at
`stage2_live`) genuinely holds there (its one lasso was a `MaxRound=2` bound artifact, §Area 1). In
other words the canonical model already encodes the CORRECT algorithm. The shipped C++ `closeBlob`
(`CasBlobInDegree.cpp` ~L225–251) diverged: it keys the "already retired?" decision on the **hash only**
(a prior retired entry for the hash takes the `settleEntry` branch and NEVER reaches the fresh-condemn
path for the current token), and the fold only visits blobs touched in the current window. No model was
faithful to that drifted code. This model is that faithful variant; the fix (`FixReCondemnCurrentToken`)
re-condemns the current token when settling a prior entry whose token differs from the present
object — i.e. it makes the code match `CaIncarnationCore`'s already-proven `GRetire`.

**Why a focused model (not a flag in `CaIncarnationCore`).** A `SabotageRetireByHash` flag was tried in the canonical model (key the retire decision on hash, not (hash, current token)). It does NOT reproduce the *permanent* orphan: `CaIncarnationCore`'s `GRetire` is **un-touch-gated** — it re-condemns any eligible blob (`present ∧ everEdged ∧ InDeg=0`) every retiring phase — so once the stale entry drops, a later round simply re-condemns the replaced token. At every `MaxRound` both the sabotaged and the unsabotaged run violate `NoLeakForever` identically (the last-round bound artifact the doc already notes), so the property cannot distinguish them. The C++ orphan is permanent ONLY because the fold is **touch-gated** (`closeBlob` visits a blob only when it has edge deltas this window; once the resurrect-replaced token's edges are folded-and-gone the fold never revisits it). The canonical model abstracts that away (idealized always-eventually-condemn GC) — which is the DEEPER reason it misses this class. A faithful port would need to add a touch-gating dimension to `GFold`/`GRetire` plus a state invariant (not round-capped liveness); that is a real structural change, so the focused model is the gate for now.

**Code currency:** the C++ fix is CURRENT and **LANDED** (branch `cas-gc-rebuild`):
`CasBlobInDegree.cpp` `closeBlob` re-condemns the CURRENT token when settling a prior retired entry
whose token differs from the present object (keyed on `(hash, current token)`, matching
`CaIncarnationCore`'s `GRetire`), with a `blob_retire_replaced` CA-log event + `CasGcRetireReplaced`
counter. The model files were removed 2026-07-22: at 194 distinct states TLC explored essentially
the single scenario that the deterministic gtests
(`CasGcLeak.ResurrectReplacedIncarnationReclaimed`, `ResurrectReplacedReclaimIsIdempotent`,
`ResurrectReplacedTokenIsCondemnedInMeta`, `src/Disks/tests/gtest_cas_gc_leak.cpp`) pin directly,
so as a regression gate the model added nothing beyond the unit tests; its lasting lesson (the
model-vs-code faithfulness gap above) is prose. Documented follow-up (non-blocker): add a
touch-gating dimension to `CaIncarnationCore` `GFold`/`GRetire` so the canonical model can
reproduce this class directly.

---

## Area 13 — Writer/GC simplification, freshness-meta, and round-defer (2026-07-06 → 2026-07-10) {#area-writer-gc-simplification}

Four models gate the writer/GC simplification effort (spec
`2026-07-09-cas-writer-gc-simplification-design.md`) and the adjacent GC round-defer and orphan-sweep
work. Two further models explored and rejected alternate freshness-meta designs, kept as historical
record (the doc's convention for rejected models, e.g. `CaB140Dangle.tla` / `CaGcCore.tla`).

### `CaGcRoundDeferCore.tla` — GC round DEFER (skip-unchanged) core {#cagcrounddefercore}

**Files:** `CaGcRoundDeferCore.tla`, `CaGcRoundDeferCore_stage1.cfg`,
`CaGcRoundDeferCore_witness_deferthenfold.cfg`, `CaGcRoundDeferCore_sab_graduate_on_stale.cfg`,
`CaGcRoundDeferCore_sab_unbounded_defer.cfg` (spec `2026-07-06-cas-gc-round-skip-unchanged-design.md`,
Phase 4 Lever A). Mirrors `CaGcAckFloorCore.tla`'s round shape (`GBegin`/`GFold`/`GComplete`, a monotone
`min_ack` abstraction).

**What it proves.** A round that would make no destructive decision may DEFER (re-adopt the sealed
in-degree snapshot) instead of rebuilding it, but a due graduation must force-fold first: `GComplete`
may physically delete a blob only when the unfolded delta carrier holds no pending add/remove touching
it (`NoOverDelete`) — the mirror of the 2026-06-27 concurrent-leader leak on the unfolded `+1` side.
Deferral itself is bounded (`deferCount < MaxDefer`, `EventuallyFolded`) so an unfolded delta is never
permanently skipped. `stage1` is clean (8,445 distinct states); `sab_graduate_on_stale` (drop the
unfolded-covers-`b` delete guard) violates `NoOverDelete`; `sab_unbounded_defer` (drop the bound)
violates `EventuallyFolded` (a permanent-skip liveness lasso); `witness_deferthenfold` confirms a real
DEFER-then-FOLD sequence is reachable (non-vacuity).

**Code currency:** CURRENT (gate for the GC round DEFER/skip-unchanged mechanism).

---

### `CaEdgeBeforeObserve.tla` — writer/GC simplification Gate A {#caedgebeforeobserve}

**Files:** `CaEdgeBeforeObserve.tla`, `CaEdgeBeforeObserve_reduced.cfg`,
`CaEdgeBeforeObserve_sab_late_edge.cfg`, `CaEdgeBeforeObserve_sab_no_adopt_check.cfg`,
`CaEdgeBeforeObserve_sab_no_k3_head.cfg`, `CaEdgeBeforeObserve_sab_no_k3_adopt_check.cfg` (Gate A of
`2026-07-09-cas-writer-gc-simplification-design.md`, Phase A).

**What it proves.** With the writer order `precommit (closure durable) → adopt/observe → promote` and
GC's `condemn → graduate(floor) → same-pass decided delete` pipeline with per-pass d-recheck, the
promote-time revalidation of TOKENED leaves is redundant, while the dedup-adoption check (K1), the
tokenless presence HEAD (K3Head), the tokenless condemned check (K3AdoptCheck), and the ORDER itself
stay load-bearing. `reduced` (no tokened revalidation) holds; the four sabotages each reproduce a
dangle: `sab_late_edge` (adoption allowed before the durable closure — the pre-B188 order),
`sab_no_adopt_check` (K1 removed), `sab_no_k3_head` (absent tokenless leaf published blind),
`sab_no_k3_adopt_check` (condemned tokenless leaf adopted at promote).

**Code currency:** CURRENT for the load-bearing half; the tokenless-leaf half has DRIFTED (audited
2026-07-22). The parts that still match the code: no promote-time revalidation of tokened leaves
(`CasPartWriteTxn.cpp:985-986` skips them as edge-protected), the edge-before-observe order
(fail-closed at `CasPartWriteTxn.cpp:331-336`), and K1 (the adopt path point-reads the per-hash
meta; `Condemned` ⇒ `ABORTED` ⇒ re-upload a fresh incarnation, `CasPartWriteTxn.cpp:298-320`). But
**K3Head and K3AdoptCheck no longer describe shipped code**: a tokenless (`adoptEvidence`) leaf is
now **trusted via its durable manifest edge** — no promote-time presence HEAD, no `loadMeta`, no
copy-forward (`CasPartWriteTxn.cpp:926,987-1004`); only a no-dep / pending-upload leaf fails closed.
The model's leaf `he` (a pre-existing unowned blob GC can condemn, so promote must HEAD +
condemned-check it) is superseded by an in-degree-pinned source (in-degree ≥ 1, not condemnable),
with fsck as the genuinely-absent backstop. So `sab_no_k3_head` / `sab_no_k3_adopt_check` are valid
negative controls for the MODEL's logic but gate a promote-time probe the code no longer performs.
Not a CODE-RISK — the safety argument moved to the in-degree invariant + manifest-trust + fsck.
Recast the tokenless leaf as source-pinned (trusted, no promote probe): scoped follow-up, not done
in this pass.

---

### `CaMetaDescriptor.tla` — writer/GC simplification Gate B, meta descriptor (v1) {#cametadescriptor}

**Files:** `CaMetaDescriptor.tla`, `CaMetaDescriptor_reduced.cfg`,
`CaMetaDescriptor_sab_a_meta_first.cfg`, `CaMetaDescriptor_sab_b_body_first.cfg`,
`CaMetaDescriptor_sab_c_blind_adopt.cfg`, `CaMetaDescriptor_sab_d_uncond_body.cfg`,
`CaMetaDescriptor_sab_e_no_claim_sweep.cfg`, `CaMetaDescriptor_sab_f_birth_adopt.cfg`,
`CaMetaDescriptor_sab_g_fresh_head.cfg`, `run_meta.sh` — **removed 2026-07-22** (its central invariant
is contradicted by the shipped code; full text in git history).

> **Removed — the shipped meta is an advisory freshness hint, not the lifecycle linearizer this
> model assumes.** The model's headline invariant `INV-META-BODY` (meta present ⇒ body present, with
> the meta as *the* linearization point and delete-top-down = meta-first) is directly contradicted
> by the code: `Formats/CasBlobMetaFormat.h` states the marker "is only a point-read hint, **not the
> linearization point** for blob lifetime … reads of the blob never consult the meta," the body's
> in-body `incarnation_tag` + exact-token delete is the real linearizer, GC deletes the **body
> first** (`CasGc.cpp:419`) and drops the meta only advisorily afterwards, and absent reads
> identically to `Clean` (no tombstone). So a `Condemned` meta legitimately outlives its deleted
> body — a state the model forbids. Keeping a model whose headline invariant holds in the model but
> is false in the code is misleading (false comfort). The meta's real behavior is already gated by
> CURRENT models: the writer adopt-gate point-read (`Condemned` ⇒ re-upload) by `CaEdgeBeforeObserve`
> (K1), and the GC condemn-marker graduation gate by `CaGcCondemnMarkerGate`; the settled v3
> freshness argument is discharged in the design against `CaIncarnationCore`. Not a CODE-RISK — the
> meta is advisory and never authority for deletion. Also removed alongside the two earlier-rejected
> meta variants (`CaMetaDescriptorRaw`, `CaMetaIncarnationKey`) and the false-green
> `CaMetaAbsenceClean`.
>
> (The model additionally had a config defect the audit surfaced: its eight `.cfg` files omit
> `CHECK_DEADLOCK FALSE`, so a spurious terminal-state deadlock in a shallow BFS branch halted TLC
> before the sabotage counterexample was reached — five of the seven negative controls silently
> reported a deadlock instead of the intended violation. Confirmed by re-running with deadlock
> checking off: `reduced` GREEN, all seven sabotages VIOLATE. Moot now that the model is removed,
> but recorded because the same omission should be checked in any surviving small model.)

**What it proved.** `INV-META-BODY` (meta present ⇒ body present) across create-bottom-up
(body then meta, If-None-Match) / delete-top-down (meta at the captured etag, then body at the
condemn-time token) discipline, with a resurrect modeled as two steps (meta CAS, then body re-upload)
and a no-claim sweep sabotage modeled as two steps (observe, then blind delete) so the crash/race
windows are explorable. Seven sabotages (a–g: meta-before-body, body-before-meta, blind adopt over
condemned, unconditional body delete after losing the meta-delete CAS, no-claim debris sweep, birth-
completion adopting the orphan body instead of resurrect-from-source, GC deleting the body at whatever
token it currently holds instead of the condemn-time token) all dangle as required.

**Code currency:** REMOVED 2026-07-22 — the shipped meta is advisory (absent ≡ `Clean`, body-first
delete, never the linearization point), so the model's `INV-META-BODY` linearizer framing is false
against the code (see the note above). Its create-bottom-up and exact-token-body-delete results did
match code, but the advisory-meta behavior is covered by `CaEdgeBeforeObserve` (K1) +
`CaGcCondemnMarkerGate`.

---

### `CaMetaDescriptorRaw.tla` — Gate B raw-body / terminal-tombstone (REJECTED by v3) {#cametadescriptorraw}

**Files:** `CaMetaDescriptorRaw.tla`, `CaMetaDescriptorRaw_reduced.cfg`,
`CaMetaDescriptorRaw_sab_meta_first.cfg`, `CaMetaDescriptorRaw_sab_blind_adopt.cfg`,
`CaMetaDescriptorRaw_sab_adopt_tomb.cfg`, `CaMetaDescriptorRaw_sab_del_notomb.cfg`,
`CaMetaDescriptorRaw_sab_resurrect_tomb.cfg`, `run_metaraw.sh` — **removed 2026-07-21** (full text
in git history).

**What it proves.** A Gate B variant that drops the envelope to RAW, immutable, write-once bodies and
makes the per-hash meta the SOLE three-state linearizer (`clean, condemned, tombstone`), with the meta
etag as the only conditional authority. The v2 correction models GC's body delete as a non-atomic
two-step (`GcClaimTombstone` then `GcDeleteBody`) and makes `tombstone` terminal — a writer observing
`tombstone` waits for `absent` and re-creates fresh, never `tombstone → clean`
(`SabResurrectFromTombstone` re-enables the un-tombstone race and breaks `INV_NO_LOSS`/`INV_NO_DANGLE`).
`reduced` is clean; the five sabotages (meta-before-body, blind adopt, adopt-over-tombstone,
delete-without-tombstone-claim, resurrect-from-tombstone) each dangle.

**Code currency:** SUPERSEDED. Raw immutable bodies (fixed etag) cannot let a resurrect displace the
body by itself, which is exactly what forced the terminal-tombstone handshake — a writer↔GC liveness
coupling. REJECTED by the v3 design (`docs/superpowers/plans/2026-07-10-cas-freshness-meta-v3.md`,
superseding `docs/superpowers/plans/2026-07-10-cas-meta-descriptor-raw-body.md`), which keeps the
settled one-key-per-hash + in-body `incarnation_tag` + exact-token BODY delete instead. Files
removed 2026-07-21; this section stays as the explored-and-rejected record.

---

### `CaMetaIncarnationKey.tla` — Gate B Option B, per-incarnation body keys (REJECTED) {#cametaincarnationkey}

**Files:** `CaMetaIncarnationKey.tla`, `CaMetaIncarnationKey_reduced.cfg`,
`CaMetaIncarnationKey_sab_reuse.cfg`, `run_inckey.sh` — **removed 2026-07-21** (full text in git
history).

**What it proves.** An alternative to the terminal-tombstone fix ("Option B"): per-incarnation body keys
(`blobs/xx/<hash>.<incarnation>`), a tombstone-free two-state meta (`clean`/`condemned`) pointing at the
current incarnation, and manifest records of `(hash, incarnation)`. GC deletes the condemned
incarnation's body by its exact key; a resurrect writes a fresh incarnation key, so GC's delete can
never hit the writer's live body — no cross-object atomicity, no tombstone, no wait. The sabotage
`SabResurrectReuseIncarnation` (resurrect reuses the condemned incarnation instead of minting a fresh
one) reintroduces the shared-key race and dangles, proving fresh-incarnation-on-resurrect load-bearing
within this design.

**Code currency:** SUPERSEDED/REJECTED. This is the generation-in-key design already rejected once
before (see `docs/superpowers/cas/01-architecture.md` §"Approaches tested and REJECTED": EBR's
`blobs/<H>/<g>` and Merkle `child_gen`): it forces a `404 → LIST` read path and propagates the
incarnation up into the manifest/parent, breaking the pure-content manifest and FUSE-readiness. Files
removed 2026-07-21; this section stays as the explored-and-rejected record.

---

### `CaManifestSweepWindow.tla` — orphan-sweep vs removal-fold wedge {#camanifestsweepwindow}

**Files:** `CaManifestSweepWindow.tla`, `CaManifestSweepWindow_reduced.cfg`,
`CaManifestSweepWindow_sab_sweep_committed.cfg`, `run_sweepwindow.sh` — **removed 2026-07-22**
(covered by the deterministic gtest, see below; full text in git history). (Task 0 of
`docs/superpowers/plans/2026-07-10-cas-freshness-meta-v3.md`; the wedge gate for the committed-removal-
scoping debt behind `gtest_cas_orphan_manifest_sweep.cpp::PendingCommittedRemovalBodyIsSkipped`).

**What it proves.** `INV_FOLD_PROGRESS`: when a COMMITTED manifest ref is dropped, its `-1` removal is
appended to the shard journal but not yet sealed by the GC fold; a promoted build has retired its
`build_seq`, so the prefix is watermark-eligible for the orphan-manifest sweep. The sweep must NOT
delete the committed body in the `dropRef → fold-seal` window — the removal-fold still needs the body
present to emit its decrement. `reduced` is clean; `sab_sweep_committed` (sweep ignores the
pending-committed-removal protection) deletes the body, so the fold can never decrement and
`INV_FOLD_PROGRESS` is violated forever.

**Code currency:** the wedge fix is CURRENT (`8606ab382aa`); the model files were removed
2026-07-22 — the interleaving space is small enough that the deterministic gtest
(`gtest_cas_orphan_manifest_sweep.cpp::PendingCommittedRemovalBodyIsSkipped`, plus ten sibling
sweep tests) covers the same scenarios, so the model added no assurance beyond the unit test.
