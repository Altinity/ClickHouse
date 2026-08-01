---
description: 'Directory index of the CAS MergeTree TLA+ models: per-model purpose, status against the shipped code, configs, and runner scripts.'
sidebar_label: 'CAS TLA+ models directory'
sidebar_position: 1
slug: /superpowers/models
title: 'CAS MergeTree — TLA+ models directory index'
doc_type: 'guide'
---

# CAS MergeTree — TLA+ models directory {#cas-tla-models-directory}

This directory holds the TLA+ formal models for the content-addressed (CAS) MergeTree feature.
This README is the complete, self-contained index: one entry per model, what it proves in plain
terms, its status against the shipped code, its config files, and its runner script.

Audit date for every status below: **2026-07-21**, verified against branch `cas-gc-rebuild`
(CAS code under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`).

A note on names: some module names carry historical bug numbers from the development era (e.g.
`CaB140DangleMerge`). The names are kept — renaming a verified model buys nothing — and the story
behind each is told in prose below.

## Conventions {#conventions}

- `<Model>.tla` — the module; `<Model>_*.cfg` — TLC configs for that module (prefix-matched, no
  sharing across modules; exception: `m_*.cfg` belong to `CaB140DangleMerge.tla`).
- `_stage*` / `_safe` / `_reduced` / `_fix` — positive gates: must pass.
- `_sab_*` / `_bug` — sabotages (negative controls): each removes one load-bearing rule and MUST
  produce a counterexample; an unexpected pass means the model lost its teeth.
- `_witness_*` — negated reachability: TLC reporting a "violation" means the state IS reachable
  (non-vacuity check).
- `run_*.sh` — TLC/Apalache runners; TLC jar expected at `../../../tmp/tla2tools.jar`
  (v2.19), Apalache at `../../../tmp/apalache/bin/apalache-mc` (0.58.0+). Two shapes exist: a
  **suite runner** owns its model's whole config list and asserts each expected outcome *by the name
  of the invariant or property it must break* (`run_mount.sh` is the reference), while the rest are
  single-config drivers taking a cfg as `$1` and asserting nothing. Prefer the first shape: a colour
  nothing checks is a colour that rots.
- `states/`, `tmp/`, `_apalache-out/` — tool scratch output, gitignored / untracked.
- `*_RESULTS.md` — supplementary raw TLC run evidence (state counts, counterexample traces) for
  the matching model. `<date>-*-RESULTS.md` — a whole-phase gate across several models
  (`2026-07-28-v9-phase-RESULTS.md` is the v9 ref-chain one).

Status legend:

- **CURRENT** — gates shipped code; the mechanism it proves is what the code does.
- **CURRENT (partial)** — the model is the live gate for part of what it proves; the superseded
  part is called out in the entry.
- **STALE** — proves the current design family but predates later amendments; kept deliberately.
- **HISTORICAL** — record of a real production-shaped bug and its fix proof; the surrounding
  mechanism has since evolved.
- **MIXED** — part of the model matches the code and part models a superseded mechanism; the entry
  says which is which.

## Audit note (2026-07-22) {#audit-2026-07-22}

Every model was checked against the shipped `cas-gc-rebuild` code (TLC health: positive configs
GREEN, sabotages VIOLATE; plus a per-model code-correspondence read). **Headline result: no
CODE-RISK** — there is no model whose proven-necessary rule the shipped code violates; wherever a
model and the code diverge, the code upholds the same safety conclusion via an equal-or-stronger
mechanism.

The `cas-gc-rebuild` branch rearchitected GC and the writer (separate ref-log objects, source-edge
runs, round-only graduation pacing, advisory freshness meta). Consequently a cluster of models that
gate *pre-rebuild concrete mechanisms* is now **MIXED or drifted** even though their safety
conclusions still hold: `CaIncarnationCore` (safety spine intact, concrete journal/fence structure
superseded), `CaGcAckFloorCore` (writer-ack graduation floor superseded by round-only pacing; its
`GRebuild` + clamp-suppression still match), `CaBuildRootPrecommit` (inline-closure + per-blob
presence mechanisms drifted to lazy-fold-with-clamp-barrier + owner-liveness), `CaEdgeBeforeObserve`
(the tokenless-leaf `K3Head`/`K3AdoptCheck` half superseded by manifest-trust), and the
sharding/fence half of `CaGcRootLocalPartManifestCore` (its `EnableSharding` arm — the positive
`stage5_sharding` gate *and* two fence-era sabotages — now crashes TLC with `CHOOSE m ∈ {}` at
`TheM` rather than running, a regression from the historical 983.9M-state run; the non-sharding
stages are unaffected and its sharding correctness is covered by `gtest_cas_gc_shard_plan.cpp`). Realigning these — trimming the ack apparatus, excising the
fence/recheck half, recasting the tokenless leaf / closure mechanism — is **deferred**: each is a
rewrite of a concurrency proof model where a modeling slip yields a false-green, so it belongs to a
careful pass with adversarial review, not an unattended edit. The current GC round's safety IS
gated by CURRENT models regardless: `CaGcRoundDeferCore`, `CaGcCondemnMarkerGate`,
`CaGcAckFloorZombie` (two-phase graduation), `CaRetiredInRun`, and the fold/orphan/attempt machinery
of `CaGcRootLocalPartManifestCore`.

## Summary table {#summary-table}

| Model | Proves / gates | Status | Runner |
|---|---|---|---|
| `CaIncarnationCore.tla` | canonical incarnation-token GC core (fold → retire → fence → recheck → exact-token delete → cascade → trim) | CURRENT (safety spine; concrete journal/fence structure superseded) | `run_tlc.sh` |
| `CaBuildRootPrecommit.tla` | adopted-blob dangle fix: precommit-first build-root reachability + fail-closed commit + inline closure recording | CURRENT conclusion (inline-closure + presence-gate mechanisms drifted → lazy-fold+clamp-barrier + owner-liveness) | `run_buildrootprecommit.sh` |
| `CaGcLeaseCore.tla` | GC leader lease: epoch-fence safety, advisory heartbeat against false steals | CURRENT | (inline TLC) |
| `CaCasMountCore.tla` | mount ownership: sticky owner, monotone epoch, observation-based lease reclaim, and the v9 recovery-generation layer (a generation captured at admission and rechecked post-I/O before every publication; a successor's `EpochSeal` as a conclusive rejection; the acked-then-lost byte comparison) | CURRENT (v9 extension 2026-07-28; gates the ref-chain implementation's `slot-occupy` / `_ckpt` / install changes) | `run_mount.sh` |
| `CaGcRootLocalPartManifestCore.tla` | root-local part-manifest GC: fold, manifest cleanup, orphan sweep, attempt scoping | CURRENT (partial: fence/recheck phases superseded by the ack-floor round) | `run_gc_partmanifest.sh` |
| `CaGcAckFloorCore.tla` | one-pass GC round, clamp suppression, disaster-recovery rebuild | MIXED: graduation gate (writer-ack floor) superseded by round-only pacing; `GRebuild` + clamp-suppression still match | `run_ackfloor.sh` |
| `CaGcAckFloorZombie.tla` | two-leader `delete_pending` two-phase graduation | CURRENT (partial: same caveat) | `run_ackfloor_zombie.sh` |
| `CaGcShardIncarnationCore.tla` | namespace-registry removal: per-shard incarnation + newborn round self-floor | CURRENT | (inline TLC) |
| `CaGcRoundDeferCore.tla` | GC round may skip an unchanged snapshot only if no destructive decision is due; deferral bounded | CURRENT | (inline TLC) |
| `CaGcCondemnMarkerGate.tla` | graduation gated on confirmed durable condemn marker | CURRENT | `run_condemnmarker.sh` |
| `CaEdgeBeforeObserve.tla` | with edge-before-observe write order, promote-time revalidation of tokened leaves is redundant | CURRENT for order + K1; K3Head/K3AdoptCheck drifted (tokenless leaf now manifest-trusted) | `run_ebo.sh` |
| `CaRetiredInRun.tla` | retired list folded into the snapshot run (two-cursor merge, coverage coherence) | CURRENT | `run_retiredinrun.sh` |
| `CaRetiredInRunFoldAbortWitness.tla` | GC freshness meta is add-only: a spare never clears a condemned marker | CURRENT | `run_foldabort_witness.sh` |
| `CaRefTableSnapshotLogCore.tla` | v9 contiguous ref stream: state-derived dense ids, every-attempt reuse rule, `_ckpt`-based recovery base + arithmetic walk, in-band `slot-occupy` epoch seal — `LatePredecessorPut` FLIPPED from rev.4 expected-fail to green, with `_sab_noseal` as the control | CURRENT (v9 rewrite 2026-07-28; gates the ref-chain implementation) | `run_refsnaplog.sh` |
| `CaRefDeltaIntakeCore.tla` | pool-wide GC fold: arithmetic walk, destructive-round frontier proof, durable hold; assumes its fresh pre-fold catalog cut | CURRENT | `run_deltaintake.sh` |
| `CaRefCatalogCore.tla` | local namespace lifecycle: create/reconcile safety, fresh opaque life ids, bounded catalog churn, and evidence + no-hold + exact-row authorization for `Removing -> absent` | CURRENT (re-scoped 2026-08-01; gates the ref-chain implementation) | `run_refcatalog.sh` |
| `CaRefPreFoldDrainCore.tla` | two-GC-actor ordering plus its two-row serial-rescan companion: adopted-parent proof, conclusive exact catalog drain, fresh post-drain cut, then fold/`REBUILD`/`DEFER`; the companion returns external resolution to a complete rescan and red-controls a non-exact CAS; damaged-state `REBUILD` restores authority without deleting catalog rows | CURRENT (new 2026-08-01; owns the cross-object removal order) | `run_prefold_drain.sh` |
| `CaRefFoldClampRecoveryCore.tla` | fold clamp always recoverable: per-log cleanup staging | CURRENT | `run_foldclamp.sh` |
| `CaRefNsCleanupStaleLeaderCore.tla` | perpetual janitor: a LIST page captures an exact physical life id, and a delayed delete never re-derives its target from a reborn logical name | CURRENT (re-scoped 2026-08-01; gates the ref-chain implementation) | `run_nscleanup_staleleader.sh` |
| `CaRefWriterCleanupCore.tla` | ref-table writer ownership lifecycle: precommit, promote, fence, successor cleanup | CURRENT | `run_refwcleanup.sh` |
| `CaRelinkConfirmCore.tla` | publish-then-confirm relink: gate 1 (exact-`ManifestRef` equality, lane quiescence, poison, mount fence) and the publish-before-confirm order, each proven load-bearing — **plus the finding that the theorem is violable independently of the protocol under an honest fold cursor** | CURRENT (gates unlanded Part B; `_main` is CONDITIONAL on `LIST` completeness) | `run_relinkconfirm.sh` |
| `CaErasureProof.tla` | rev.7 natural `Vanished(erased)` proof soundness: writer paths closed by op-gate + guard counter + LIST-reset + grace ([D1] grace proven load-bearing); two GC-side windows found — evidence in the decision to excise the natural-erasure stack from v1 | HISTORICAL (design excised before activation) | `run_erasureproof.sh` |
| `CaDiskLifecycle.tla` | rev.8 FORGET-only v1 lifecycle: one-way-ness, the as-built `FORGET` protocol (trip#2 sufficiency, earned farewell, first-terminal-wins), the [C1] GC self-exit-on-Vanished, the [M1] intent-bail; Task-15 gate | CURRENT | `run_disklifecycle.sh` |
| `CaB140DangleMerge.tla` (+ `m_*.cfg`) | journal-trim dangle across a lease handoff: trim-gate + cursor-in-snap jointly necessary | HISTORICAL | (inline TLC) |

## Model groups {#model-groups}

### GC core and proofs {#group-gc-core}

- **`CaIncarnationCore.tla`** — the canonical adversarial GC core: concurrent writers and GC
  leaders, split-brain, debris classification, full-GC cut, resurrect/overwrite, trees with atomic
  cascade, namespace registration and evidence staleness. Invariants: no committed reference to an
  absent object (`INV_NO_DANGLE`), no reachable object lost (`INV_NO_LOSS`), no deleted token ever
  current again (`INV_NO_RETURN`), journal trim never outruns the fold cursor
  (`INV_JOURNAL_COVERAGE`); 11 sabotages. Key rules it forced: the fence writes to every manifest;
  recheck requires the fold to have advanced through the fence; deletes are exact-token; cascade is
  atomic with the delete; the registry fence must use the committed (not fold-time) namespace
  universe; stale dependency evidence must be re-observed before publish. Configs: `_stage1..6*`,
  `_hunt_*`, `_reval_stage2`, `_sab_*`.
- **`CaGcAckFloorCore.tla`** / **`CaGcAckFloorZombie.tla`** — the one-pass GC round and its
  two-leader hardening. The round pipeline (`GBegin`/`GFold`/`GComplete`), the clamp-suppression
  guard (a pass that had to hold back an unreadable shard makes no destructive decision), the
  disaster-recovery rebuild (restart with an empty retired list and a round minted above every
  surviving mount's acknowledgement), and the two-phase graduation (`delete_pending` published by
  a round-CAS before any physical delete, so a deposed leader's stale pass cannot delete a live
  blob) are all CURRENT and match the shipped GC. The other half these models prove — writers
  advertising an observed round through heartbeats, with graduation gated on the minimum
  acknowledgement — was later removed from the code (the per-hash freshness meta made the
  writer-side acknowledgement unnecessary; graduation now paces on GC rounds alone), so those
  parts stand as the record of a mechanism that worked but was simplified away.
- **`CaGcRootLocalPartManifestCore.tla`** — the largest model in the corpus (28 sabotages):
  part manifests owned by refs, precommit and missing-body states, owner transitions, orphan
  sweep, token-diff discovery, lazy trim, sharded reducers, attempt-scoped generation visibility,
  plus two later regression gates: an abandoned precommit provably dead by the build watermark
  must still be reclaimed even on a content-static shard the fold would otherwise skip; and a ref
  may own at most one committed manifest (a promote must fail closed instead of overwriting a
  different committed binding). The per-round all-shard fence and fold-through-fence recheck it
  also models were replaced by the ack-floor round; those controls are kept as evidence for why
  patching that mechanism was not enough.

### Focused GC bug gates {#group-focused-gates}

- **`CaGcLeaseCore.tla`** — GC leadership is guarded by an atomic single-CAS steal with a fence
  epoch, which alone prevents two leaders committing at the same epoch. The model adds the
  liveness half: without an advisory heartbeat, a steal can fire against an alive leader whose
  sequence number is legitimately frozen for the duration of a long round; the heartbeat is the
  minimal addition that eliminates such false steals.
- **`CaGcShardIncarnationCore.tla`** — proves the namespace registry could be deleted: its safety
  role is fully replaced by two coordinates — a durable, never-reused per-shard incarnation
  (prevents ABA confusion of a delete-and-recreate at the same path) and a newborn shard born
  fenced to the current GC round (closes the publish race the registry used to close). Neither
  coordinate alone suffices; reclaim must also wait until the shard journal is fully folded. Raw
  TLC evidence: `CaGcShardIncarnationCore_RESULTS.md`.
- **`CaGcRoundDeferCore.tla`** — a GC round that would make no destructive decision may re-adopt
  the sealed in-degree snapshot instead of rebuilding it, but a due graduation must force a fold
  first (no physical delete while an unfolded delta could still touch the blob), and deferral is
  bounded so an unfolded delta is never skipped forever. Raw TLC evidence:
  `CaGcRoundDeferCore_RESULTS.md`.
- **`CaGcCondemnMarkerGate.tla`** — found by an external code review (2026-07-17): the GC swallows
  failures of the asynchronous condemn-marker write, while the round commits the retired entry
  regardless. The per-hash marker is the writer's adopt gate, so a lost marker lets a writer adopt
  the very token a later graduation deletes — a dangling manifest. The proven fix: graduation to
  `delete_pending` requires confirmed durable Condemned evidence (write-completion callback or a
  synchronous meta re-read); otherwise the entry is carried to the next round, fail-safe. Landed
  as `Gc::scheduleCondemnMarkerWrite` / `noteCondemnMarkerDurable` and the confirmed-markers set.

### Writer protection and freshness meta {#group-writer-meta}

- **`CaBuildRootPrecommit.tla`** — the live protection against the adopted-blob dangle: one build
  writes a blob, a second build adopts (deduplicates against) it, the first build dies, GC deletes
  the now-unowned blob, and the second build's commit blindly publishes a manifest referencing it.
  The model proves the 2×2 necessity/sufficiency matrix of the fix: a durable precommit edge must
  make the adopter's build root structurally reachable BEFORE relying on the adoption, AND the
  commit must re-check presence of the whole closure and fail closed — each half alone still
  dangles. A third finding: the precommit must record its blob closure inline, from the staged
  structure in memory; recording it lazily from a tree object read at GC time records an empty
  closure when the tree is already absent, and the blob then leaks forever.
- **`CaEdgeBeforeObserve.tla`** — with the writer order "precommit with durable closure, then
  adopt/observe, then promote" and a GC pipeline whose deletes are decided in the same pass with a
  per-pass re-check, re-validating already-tokened leaves at promote time is redundant and was
  removed from the code. The dedup-adoption check, the presence HEAD for tokenless leaves, the
  condemned check on tokenless adoption, and the order itself each remain load-bearing — dropping
  any one of them dangles.
### Retired-in-run family {#group-retired-in-run}

The separate durable retired list was folded into the snapshot run itself: condemned state rides
the source-edge run as rows with a condemned summary, cutting the round from three cursors to two.

- **`CaRetiredInRun.tla`** — the merge gate: settlement (re-delete, spare, graduate, carry) inside
  the snapshot run, a fold-read coverage floor so a round never settles against a shorter read
  than it adopted, monotone per-blob token mint, and the one-round staleness window the
  edge-before-observe writer order permits. Sabotages: trusting an in-memory token, reusing an
  attempt artifact, and skipping pacing each break a distinct invariant.
- **`CaRetiredInRunFoldAbortWitness.tla`** — two GC leaders in flight, with split actions so an
  older leader's pre-CAS delete can interleave with a newer leader's spare decision. Proves the GC
  freshness meta must be add-only: a spare leaves the condemned marker in place, and only a
  token-displacing writer publishes clean. The decisive sabotage shows the weaker fix — clearing
  the marker after the winning CAS — is still unsafe, so add-only is required, not just ordering.
  This witness also later refuted an attempted revival of marker-clearing (see removed
  `CaMetaAbsenceClean` below).

### Ref-table snapshot + log family {#group-ref-table}

Ref tables (the mutable name → manifest bindings) are persisted as an immutable snapshot plus an
append-only log of ref transactions with strictly increasing ids; a reader recovers with one
ordered scan and cleanup deletes only what it observed durable. The migration is shipped
(`Pool/CasRefProtocol.*`, `Formats/CasRefLogFormat.*`, `CasRefSnapshotFormat.*`, `CasRefLedger.*`).

- **`CaRefTableSnapshotLogCore.tla`** — the core protocol for one namespace, rewritten for v9
  (spec `2026-07-27-cas-ref-chain-complete-cut-design.md`): ids are per-namespace CONTIGUOUS and
  derived from state, an id is freed only under the every-attempt rule, recovery point-reads its
  base from `_ckpt` and walks the tail by arithmetic, and the walk ends by OCCUPYING the next slot
  with an in-band epoch seal. Because a listing is never consumed as truth, the hidden-hole defect
  that motivated v9 is reproduced only as a sabotage. **`LatePredecessorPut` is flipped from rev.4's
  expected-fail to a green proof**, with `_sab_noseal` keeping the old counterexample runnable as
  the control. Sabotages cover the every-attempt rule, rev.4's "safe id gap" allocator, seal
  occupancy, conditional-create-as-fence, folding the hint, both `_ckpt` deletion gates and the
  `_ckpt` semantic-max merge — the last of which loses data SILENTLY when skipped. Full verdicts,
  traces and the retired rev.4/rev.6 configs: `CaRefTableSnapshotLogCore_RESULTS.md`.
- **`CaRefDeltaIntakeCore.tla`** — the GC's fold over ref deltas, POOL-WIDE (v9): two namespaces
  with contiguous ids share one blob, so the oracle is "was an acked `+1` still unaccounted when
  the blob was deleted", which no per-namespace invariant can see. The LIST is a zero-trust hint
  consumed nowhere for correctness; the fold advances by arithmetic point reads; a destructive
  round needs a frontier proof for EVERY namespace; an impossible shape HOLDS the namespace
  durably, and REBUILD must carry the hold. Reproduces the r7-1 (cross-namespace hidden `+1`) and
  r8 (REBUILD forgets the hold) blockers as counterexamples, and records one residual exposure of
  v9 as a runnable witness. Full verdicts and traces: `CaRefDeltaIntakeCore_RESULTS.md`.
- **`CaRefCatalogCore.tla`** — the local namespace lifecycle: one logical name lived repeatedly
  through `Creating`, `Live`, `Removing`, and absence. Fresh opaque life ids make surviving physical
  residue inert and keep the catalog bounded under churn. Creation and reconciliation retain their
  fence and exact-token gates. Removal's local authorization is factored into one invariant: the
  adopted row has matching positive terminal evidence, carries no hold, and the mutation deletes the
  complete exact `Removing` row it observed. Four isolated sabotages prove each part load-bearing.
  This model deliberately does not own the temporal relation between that proof and a later adopted
  seal; that cross-object order belongs only to `CaRefPreFoldDrainCore`. Full verdicts and traces:
  `CaRefCatalogCore_RESULTS.md`.
- **`CaRefPreFoldDrainCore.tla`** — the focused two-GC-actor proof for catalog-only pre-fold drain.
  After acquiring the lease, an invocation reads the authoritative adopted parent, resolves every
  eligible exact catalog CAS conclusively, and takes a fresh catalog cut before ordinary fold,
  `REBUILD`, or `DEFER`. An ambiguous result cannot be treated as completion while the same exact row
  may remain. A stale actor's late request cannot delete against a successor hold, and a missing or
  undecodable `gc/state` authorizes only authority-restoring `REBUILD`, never a catalog delete based
  on a seal found by LIST. The takeover witness shows a successor helping an ambiguous predecessor
  to convergence. Its `CaRefPreFoldDrainAllRowsCore` companion independently proves that a
full-catalog token invalidated by one exact delete or external resolution forces a complete serial
rescan before the next candidate or successor decision. `CaRefDeltaIntakeCore` assumes this cut at
its boundary; it is not the provenance proof. Full verdicts and traces: `CaRefPreFoldDrainCore_RESULTS.md`.
- **`CaRefFoldClampRecoveryCore.tla`** — a clamped fold (a log held back at an unreadable body)
  must stay recoverable: body tokens named by a log's removal records may join the round's cleanup
  set only once the WHOLE log folds, and a clamp discards the log's staged tokens. Committing at
  edge granularity instead deletes a body the clamped log still needs, and every later re-fold
  then clamps on the missing body — a permanent, pool-wide destructive freeze.
- **`CaRefNsCleanupStaleLeaderCore.tla`** — the perpetual `cas/ns/` janitor's delayed-delete proof.
  A LIST page captures the physical life id from a returned key, and a later catalog cut nominates
  that id as foreign. The deletion may resume after same-name rebirth, but it still targets the
  captured physical id rather than resolving the logical name again. Fresh life ids and physical-id
  capture are independently load-bearing: `_sab_noincarnation` and `_sab_rederive` each delete live
  successor data. Pre-fold catalog ordering is out of scope here and belongs to
  `CaRefPreFoldDrainCore`; local catalog deletion proof belongs to `CaRefCatalogCore`.
- **`CaRefWriterCleanupCore.tla`** — the writer-side ownership lifecycle for one table: a build
  precommits (owns its precommit record), promote atomically removes the precommit and installs
  the committed owner, gated on the current epoch; failed builds are cleaned in the order
  remove-then-retire; a successor fences a new epoch and removes stale precommits by exact
  identity, bounded. Invariants: no wrongful reclaim of a live build's objects, promote never
  leaves a ref ownerless, retire only after removal, namespace removal complete.

### Fetch handoff: publish-then-confirm relink {#group-relink-confirm}

- **`CaRelinkConfirmCore.tla`** — the Task-9 gate for the publish-then-confirm relink protocol
  (spec `2026-07-23-cas-fetch-handoff-publish-confirm-design.md` rev.5, Part B). A sender ref lane
  with a durable journal, an in-memory committed row that may lag it (the post-durable-PUT window),
  an admission queue, a leader tenure, an apply-pending poison state and a mount fence; a receiver
  running publish (durable `+1`) → confirm → promote, or a durable releasing abort; and a GC round
  with a per-namespace fold cursor and condemn → `delete_pending` → delete graduation with sparing
  on positive in-degree. Proves `ConfirmedRelinkNeverDangles`, with five sabotages each removing one
  rule: gate 1's exact-`ManifestRef` equality (an ABA via a repoint answers *yes* by name),
  lane quiescence, the poison state (separately load-bearing — after a poisoned apply the lane looks
  perfectly quiescent), the mount-fence/current-writer check, and the publish-BEFORE-confirm order.
  Four `_witness_*` configs pin non-vacuity, including that the theorem's antecedent and physical
  deletion are both reachable.
  **Read the headline finding before trusting the green run:** the fold cursor is modelled honestly
  — it advances over what a round OBSERVED, and GC discovers ref-log transactions by a paginated
  `LIST` with no completeness proof. `_sab_holeylist` keeps every confirm rule intact and allows
  exactly ONE incomplete page in the whole behaviour, and the theorem still breaks: the omitted `+1`
  sinks below its namespace cursor and is never folded again, so GC deletes a deduplicated blob a
  confirmed, promoted manifest references. That is BACKLOG
  `{#list-as-journal-dataloss-2026-07-25}` mechanised. `_main` therefore runs with `MaxHoles = 0`
  and must be read as "the confirm protocol adds no new dangle path", not "a confirmed relink cannot
  dangle". Full run evidence and traces: `CaRelinkConfirmCore_RESULTS.md`.

### Mount ownership {#group-mount}

- **`CaCasMountCore.tla`** — mount ownership over a shared server-root object: the owner is
  sticky (no foreign server auto-takes-over an expired mount), the durable epoch counter is a
  strict monotone ceiling that is never reset, and a superseded actor makes no further mutations.
  Extended for lease-boundary exclusivity (2026-07-14): reclaim of an expired mount is
  observation-based — the reclaimer must observe a stable holder token over a full lease-plus-
  drift window measured on its OWN clock (one sabotage reproduces the bug of trusting the foreign
  wall clock in the mount object), and the reclaim installs the successor's own body, matching the
  shipped `CasServerRoot.cpp`. Extended again for the 2026-07-24 fence-not-rescue gate (an ungated
  `WipeEpoch` losing the durable epoch object, plus the guarded re-mint branches
  `RemintEpoch`/`RemintEpochDecom`).
  **Extended 2026-07-28 for the v9 recovery-generation layer** (spec
  `2026-07-27-cas-ref-chain-complete-cut-design.md` §3 "Recovery ownership", §9's r9-5): this module
  already owned the mount-fence generation itself, and v9 adds the consumer side — an operation
  captures the generation at admission (`recGen` for a recovery, `wedgeGen` for a wedged lane per
  INV-1's every-attempt rule) and must re-present it, with a recheck post-I/O immediately before
  every publication. Proves three things the rest of the suite could not: an old recovery's result
  returning after a self-remount is refused (`_sab_staleinstall`); a dead lane's conditional create
  cannot be admitted under the successor's generation (`_sab_wedgeretryoldgen`); and an `Occupied`
  `slot-occupy` result must be resolved BY COMPARING BYTES, or the lane acks an operation nothing
  ever wrote (`_sab_slotnocompare` → the one new invariant, `AckedOpsAreDurable`). It also carries
  two hand-offs from `CaRefTableSnapshotLogCore`, which is structurally unable to express either:
  that acked-then-lost direction, and INV-2's "`Occupied` with an `EpochSeal` terminates the walk"
  branch reached by a *concurrent* recoverer (`_witness_sealrejected`). An operation's identity is
  `(actor, generation, op)` — the op component is load-bearing and was added in a review fix round:
  without it two operations admitted at the same generation aliased, and the honest configuration
  could ack the second on the first one's bytes with the invariant unable to see it, so the compare
  only ever proved its *generation* half. The three `_sab_*_strictorder` variants re-run each
  sabotage under a state constraint pruning the model's pre-existing epoch-0 bootstrap mount (a state
  the product's strict order never reaches), which is what shows these reds are about a generation
  *transition* rather than about the bootstrap value.
  Configs: `_stage1` (legacy green gate), `_v9_recoverygen` (the v9 green gate), `_rev6_observe`
  (drift-aware, known not to complete in an interactive budget — pre-existing, `SLOW=1`),
  `_sab_adoptwedge`, `_sab_epochreset`, `_sab_fenceresurrect`, `_sab_foreigntakeover`,
  `_sab_wallclockreclaim`, `_sab_epochwipelive`, `_sab_decomblindbypass`, `_sab_staleinstall`,
  `_sab_wedgeretryoldgen`, `_sab_slotnocompare`, their three `_strictorder` variants, and seven
  `_witness_*` reachability checks. Run evidence, state counts and traces:
  `CaCasMountCore_RESULTS.md`.

### rev.7/rev.8 disk lifecycle: FORGET protocol (+ the excised erasure proof) {#group-rev7-lifecycle}

Gates for the "throw-when-uncertain" disk-lifecycle redesign
(spec `2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md`, Tasks 1-11; scope narrowed
2026-07-23 to FORGET-only v1 — the natural `Vanished(erased)` proof stack was excised). Full run
evidence and trace analysis: `.superpowers/sdd/tla-rev7-report.md`.

- **`CaDiskLifecycle.tla`** — THE Task-15 gate: the v1 lifecycle state machine
  (`Vanished` = `Replaced` | `Forgotten`; `IdentityLost` on authoritative sentinel absence) + the
  as-built `SYSTEM CONTENT ADDRESSED FORGET` step order (`Pool::forgetDisk`), concurrent with
  keeper trips, the self-remount thread (whose in-flight attempt may complete a full reclaim after
  the terminal intent is published — the [M1] step-0 intent-bail stops new attempts, not one
  mid-flight), the natural `Replaced` promotion, the GC scheduler loop with the [C1]
  self-exit-on-Vanished fix, and the `GC STOP`/`GC START` verbs under `lifecycle_mutex`. Proves:
  FORGET ends with the fence latched and a fully-terminal state under all interleavings (the
  second `tripMountLost` is exactly what closes the join-window reclaim race — `_sab_notrip2`
  reproduces Task 10's RED demo mechanically); the farewell is written only after a real drain
  (`_sab_unearnedfarewell` red); `IdentityLost`/`Vanished` are one-way; terminal states imply a
  latched fence; FORGET leaves GC destroyed and unrestartable; a started FORGET always completes
  (liveness under fairness), including when a racing natural promotion wins `enterVanished` first
  (first-terminal-wins is by design; `_witness_racedreplaced` shows the race is real); and once
  `Vanished`, the GC scheduler eventually stops ticking (`_sab_nogcselfexit` red reproduces the
  pre-[C1] bug as a liveness lasso).
- **`CaErasureProof.tla`** — HISTORICAL: soundness analysis of the natural `Vanished(erased)`
  promotion (spec §2 [C2][C3][D1]), the design excised from v1 before it ever activated in
  production — this model's GC-side traces are part of the evidence behind that decision. Verdict
  preserved for a possible v2: the writer machinery (op-gate `Live`-only admission, the op-scoped
  `DurableRequestGuard` counter, the LIST/streak reset discipline) is sound with the [D1] grace
  proven load-bearing (`_sab_nograce` red: a zombie request lands after the second sample), and
  two real GC-side windows exist (a fresh scheduler's round creating `gc/state` between the final
  LIST and the `round_in_flight` read; out-of-round heartbeat pulses) with the `Live`-gate fix
  direction validated green (`_fix_gclivegate`). Any v2 revival must re-run and extend this model.

### Historical records (kept) {#group-historical}

- **`CaB140DangleMerge.tla`** (+ `m_both_buggy.cfg`, `m_cursorskip.cfg`, `m_trimonly.cfg`,
  `m_merged.cfg`) — documents one real incident from the era when GC state lived in a mutable
  per-shard manifest with a separately committed fold cursor: a leader folded a new part's edge
  into its in-memory state, trimmed the journal against that in-memory cursor before the snapshot
  was durable, then lost its lease — the successor rebuilt from the empty committed snapshot,
  gap-skipped the trimmed journal entry, and deleted a blob a live part still referenced. The 2×2
  fix proof: the journal may be trimmed only up to the cursor carried INSIDE the committed
  snapshot (trim-gate), and the cursor must be committed atomically with the edges
  (cursor-in-snap); each half alone still loses data, both together are clean. The
  cursor-inside-the-snapshot principle carries forward into the ref snapshot + log design.

## Removed models {#removed-models}

The following models gated superseded or rejected designs, or added no assurance beyond the
deterministic C++ unit tests covering the same scenarios, and were removed during the 2026-07
model audit — one commit per model, motivation in each commit message; full text remains in git
history:

| Model | Why removed |
|---|---|
| `CaGcCore.tla` | the original epoch-based-reclamation GC design (a global epoch counter with per-writer pins); superseded by the incarnation-token design (`CaIncarnationCore.tla`); no epoch machinery exists in code |
| `CaB140Dangle.tla` | first reproduction of the journal-trim dangle, built with producer behaviors the real code never had |
| `CaB140DangleFaithful.tla` | refutation companion of the incident above: with faithful producers the first-phase mechanism is clean — a negative result about a mechanism that no longer exists at all; the incident record and fix proof live in `CaB140DangleMerge.tla` |
| `CaResurrectLiveness.tla` | modeled a condemn-time guard blocking GC from re-condemning a build's freshly-owned incarnation; that guard was never implemented — the shipped protection is precommit-first reachability (`CaBuildRootPrecommit.tla`) |
| `CaBuildWatermark.tla` | modeled a per-candidate blob-guard (a minimum-active-build watermark protecting in-flight builds' blobs from condemnation); that whole mechanism was replaced by precommit-first reachability; the surviving lemma — build sequence numbers must come from a monotone counter — lives on in precommit reclaim (`CasGc.cpp`) |
| `CaBuildWatermarkNum.tla` | numeric companion of `CaBuildWatermark.tla`, same supersession |
| `CaMetaDescriptorRaw.tla` (+ `run_metaraw.sh`) | explored raw immutable write-once bodies with a three-state meta as sole linearizer; rejected — a fixed-etag raw body cannot be displaced by a resurrect, forcing a terminal-tombstone handshake that couples writer liveness to GC |
| `CaMetaIncarnationKey.tla` (+ `run_inckey.sh`) | explored per-incarnation body keys; rejected — reintroduces the incarnation-in-the-key design rejected earlier in the project (a 404-then-LIST read path, incarnation leaking into manifests, breaking pure-content manifests) |
| `CaMetaAbsenceClean.tla` (+ `run_metaabsence.sh`) | gated a "meta absence means clean" tombstone-only variant whose heal transition clears a condemned marker in place; blocked — that is exactly the marker-clearing shown unsafe by `CaRetiredInRunFoldAbortWitness.tla` (add-only meta), and the model's own green run rested on a premise refuted by the code's token-preserving adoption path; no such code landed |
| `CaManifestSweepWindow.tla` (+ `run_sweepwindow.sh`) | proved the orphan sweep must skip a committed body whose removal record is not yet sealed by the fold (deleting it early wedges the removal-fold forever); the interleaving space is tiny and the deterministic unit test `CasOrphanManifestSweep.PendingCommittedRemovalBodyIsSkipped` (plus ten sibling sweep tests) covers the same scenarios — the model added no assurance beyond them |
| `CaGcResurrectReuploadOrphan.tla` | reproduced a real leak (a condemned blob replaced by a resurrect re-upload was never re-condemned, because the fold keyed the decision on hash alone and only revisits blobs touched in the current window) and proved the fix: settle the stale entry AND re-condemn the current token. The fix landed in `closeBlob` (`CasBlobInDegree.cpp`) and is pinned by the deterministic `CasGcLeak.ResurrectReplaced*` unit tests; at 194 distinct states the model explored essentially that one scenario, adding nothing beyond the tests |
| `CaIncarnationProofCore.tla` (+ `Apalache.tla`, `run_apalache.sh`) | an Apalache-checked inductive invariant for the single-leader, token-only fragment of the GC core; stale (predated the namespace-registry and evidence-staleness amendments — those are covered by `CaIncarnationCore.tla`) AND unverifiable here (no Apalache binary installed). A stale inductive proof of a superseded fragment that cannot be re-checked is false comfort; to revive it, install Apalache and re-derive the invariant against the current `CaIncarnationCore.tla` |
| `CaGcIndegRefoldCore.tla` | proved the completion-seal cursor must advance past what recheck already folded, to stop a **non-idempotent integer** in-degree stream going negative. The shipped fold (`CasBlobInDegree.cpp:380-389`) computes in-degree by an **idempotent** two-cursor presence-set merge (a `uint64_t` surviving-edge count that cannot underflow), so the integer-underflow hazard is structurally impossible and the model describes a design the code abandoned — retired like the EBR `CaGcCore.tla` |
| `CaMetaDescriptor.tla` (+ `run_meta.sh`) | Gate B v1 per-hash freshness meta. Its headline invariant `INV-META-BODY` (meta ⇒ body, meta as the lifecycle linearizer, meta-first delete) is contradicted by the shipped code: `CasBlobMetaFormat.h` states the marker is "only a point-read hint, not the linearization point … reads never consult the meta"; GC deletes the body first and drops the meta advisorily, absent ≡ Clean (no tombstone), so a Condemned meta legitimately outlives its deleted body — a state the model forbids. Keeping a model whose headline invariant is false in the code is false comfort. The meta's real role is gated by `CaEdgeBeforeObserve` (K1 adopt-gate) + `CaGcCondemnMarkerGate`. (Also had a config defect: its 8 cfgs omit `CHECK_DEADLOCK FALSE`, so five of seven sabotages silently reported a spurious deadlock instead of the intended violation — verified by re-running with deadlock checking off.) |
| `CaIncarnationCore.pdf`, `CaIncarnationCore.toolbox/` | generated TLA+ Toolbox pretty-print artifacts (regenerable from the module) |
