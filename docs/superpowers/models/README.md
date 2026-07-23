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
- `run_*.sh` — thin TLC/Apalache wrappers; TLC jar expected at `../../../tmp/tla2tools.jar`
  (v2.19), Apalache at `../../../tmp/apalache/bin/apalache-mc` (0.58.0+).
- `states/`, `tmp/`, `_apalache-out/` — tool scratch output, gitignored / untracked.
- `*_RESULTS.md` — supplementary raw TLC run evidence (state counts, counterexample traces) for
  the matching model.

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
| `CaBuildRootPrecommit.tla` | adopted-blob dangle fix: precommit-first build-root reachability + fail-closed commit + inline closure recording | CURRENT conclusion (inline-closure + presence-gate mechanisms drifted → lazy-fold+clamp-barrier + owner-liveness) | (inline TLC) |
| `CaGcLeaseCore.tla` | GC leader lease: epoch-fence safety, advisory heartbeat against false steals | CURRENT | (inline TLC) |
| `CaCasMountCore.tla` | mount ownership: sticky owner, monotone epoch, observation-based lease reclaim | CURRENT | `run_mount.sh` |
| `CaGcRootLocalPartManifestCore.tla` | root-local part-manifest GC: fold, manifest cleanup, orphan sweep, attempt scoping | CURRENT (partial: fence/recheck phases superseded by the ack-floor round) | `run_gc_partmanifest.sh` |
| `CaGcAckFloorCore.tla` | one-pass GC round, clamp suppression, disaster-recovery rebuild | MIXED: graduation gate (writer-ack floor) superseded by round-only pacing; `GRebuild` + clamp-suppression still match | `run_ackfloor.sh` |
| `CaGcAckFloorZombie.tla` | two-leader `delete_pending` two-phase graduation | CURRENT (partial: same caveat) | `run_ackfloor_zombie.sh` |
| `CaGcShardIncarnationCore.tla` | namespace-registry removal: per-shard incarnation + newborn round self-floor | CURRENT | (inline TLC) |
| `CaGcRoundDeferCore.tla` | GC round may skip an unchanged snapshot only if no destructive decision is due; deferral bounded | CURRENT | (inline TLC) |
| `CaGcCondemnMarkerGate.tla` | graduation gated on confirmed durable condemn marker | CURRENT | `run_condemnmarker.sh` |
| `CaEdgeBeforeObserve.tla` | with edge-before-observe write order, promote-time revalidation of tokened leaves is redundant | CURRENT for order + K1; K3Head/K3AdoptCheck drifted (tokenless leaf now manifest-trusted) | `run_ebo.sh` |
| `CaRetiredInRun.tla` | retired list folded into the snapshot run (two-cursor merge, coverage coherence) | CURRENT | `run_retiredinrun.sh` |
| `CaRetiredInRunFoldAbortWitness.tla` | GC freshness meta is add-only: a spare never clears a condemned marker | CURRENT | `run_foldabort_witness.sh` |
| `CaRefTableSnapshotLogCore.tla` | ref-table snapshot + append-only log protocol; coverage-at-birth mount seal | CURRENT | `run_refsnaplog.sh` |
| `CaRefDeltaIntakeCore.tla` | GC ref-intake pagination; cursor adoption atomic with the fold commit | CURRENT | `run_refintake.sh` |
| `CaRefFoldClampRecoveryCore.tla` | fold clamp always recoverable: per-log cleanup staging | CURRENT | `run_foldclamp.sh` |
| `CaRefNsCleanupStaleLeaderCore.tla` | stale-leader namespace-cleanup pass aborts on completed-marker observation | CURRENT | `run_nscleanup_staleleader.sh` |
| `CaRefWriterCleanupCore.tla` | ref-table writer ownership lifecycle: precommit, promote, fence, successor cleanup | CURRENT | `run_refwcleanup.sh` |
| `CaErasureProof.tla` | rev.7 `Vanished(erased)` erasure-proof soundness: writer paths closed by op-gate + guard counter + LIST-reset + grace ([D1] grace proven load-bearing); two GC-side windows found + fix candidate validated | CURRENT (documents two open GC-side findings) | `run_erasureproof.sh` |
| `CaDiskLifecycle.tla` | rev.7 lifecycle one-way-ness + the as-built `FORGET` protocol: trip#2 sufficiency, earned farewell, GC stop/start serialization, first-terminal-wins race, FORGET always completes | CURRENT | `run_disklifecycle.sh` |
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

- **`CaRefTableSnapshotLogCore.tla`** — the core protocol for one table: the true history is the
  id-ordered sequence of immutable transactions; a snapshot captures a replay prefix; recovery is
  a single ordered scan (log before snapshot, resume after the last returned key, bounded restart
  on a vanished object); a removed table may be recreated only after a durable Completed marker.
  A later extension seals a successor's mount coverage at birth against a stale predecessor's
  late-landing write: the late object still lands physically but is provably folded out of every
  reader started after the drop. Sabotages cover deleting before the snapshot is durable, treating
  a vanished object as corruption, recreating before the Completed marker, and remounting with the
  old epoch. The `_latepred` configs document the pre-seal cross-epoch limitation as an expected
  fail.
- **`CaRefDeltaIntakeCore.tla`** — the GC's paginated intake of ref deltas: two tables sharing one
  lexically ordered keyspace, strictly increasing durable ids with at most one unresolved append.
  Proves resume-after-returned-key pagination misses nothing, cursor adoption is atomic with the
  fold commit, and cleanup requires BOTH cursor and snapshot coverage — each rule broken
  individually produces a counterexample.
- **`CaRefFoldClampRecoveryCore.tla`** — a clamped fold (a log held back at an unreadable body)
  must stay recoverable: body tokens named by a log's removal records may join the round's cleanup
  set only once the WHOLE log folds, and a clamp discards the log's staged tokens. Committing at
  edge granularity instead deletes a body the clamped log still needs, and every later re-fold
  then clamps on the missing body — a permanent, pool-wide destructive freeze.
- **`CaRefNsCleanupStaleLeaderCore.tla`** — a GC leader that stalls mid-round while owing a
  namespace-cleanup delete pass must not resume blind: a successor may have completed the cleanup
  and a writer may have recreated the namespace at the same keys. The pass must re-read the GC
  state (abort unless its round is still current), abort on the cleanup-completed marker, and
  epoch-filter its deletes; dropping those guards lets the straggler reclaim the recreated
  namespace's live objects.
- **`CaRefWriterCleanupCore.tla`** — the writer-side ownership lifecycle for one table: a build
  precommits (owns its precommit record), promote atomically removes the precommit and installs
  the committed owner, gated on the current epoch; failed builds are cleaned in the order
  remove-then-retire; a successor fences a new epoch and removes stale precommits by exact
  identity, bounded. Invariants: no wrongful reclaim of a live build's objects, promote never
  leaves a ref ownerless, retire only after removal, namespace removal complete.

### Mount ownership {#group-mount}

- **`CaCasMountCore.tla`** — mount ownership over a shared server-root object: the owner is
  sticky (no foreign server auto-takes-over an expired mount), the durable epoch counter is a
  strict monotone ceiling that is never reset, and a superseded actor makes no further mutations.
  Extended for lease-boundary exclusivity (2026-07-14): reclaim of an expired mount is
  observation-based — the reclaimer must observe a stable holder token over a full lease-plus-
  drift window measured on its OWN clock (one sabotage reproduces the bug of trusting the foreign
  wall clock in the mount object), and the reclaim installs the successor's own body, matching the
  shipped `CasServerRoot.cpp`. Configs: `_stage1`, `_rev6_observe` (main green gate),
  `_sab_adoptwedge`, `_sab_epochreset`, `_sab_fenceresurrect`, `_sab_foreigntakeover`,
  `_sab_wallclockreclaim`, and four `_witness_*` reachability checks.

### rev.7 disk lifecycle: erasure proof + FORGET {#group-rev7-lifecycle}

Gates for the rev.7 "throw-when-uncertain, truth-when-proven" disk-lifecycle redesign
(spec `2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md`, Tasks 1-11). Full run
evidence and trace analysis: `.superpowers/sdd/tla-rev7-report.md`.

- **`CaErasureProof.tla`** — soundness of the natural `Vanished(erased)` promotion (spec §2
  [C2][C3][D1]): when the observer declares "verified: pool prefix empty", the prefix is empty and
  stays empty. The observer's sample is split into its two non-atomic halves (the gate's LIST, then
  the qualification reads) exactly as the code orders them — the load-bearing non-atomicity. The
  writer machinery (op-gate `Live`-only admission, the op-scoped `DurableRequestGuard` counter, the
  LIST/streak reset discipline, the [D1] grace) is proven closed (`_nogc_grace` green), the grace is
  proven load-bearing (`_sab_nograce` red: a zombie request — guard released on timeout, request
  still in flight — lands after the second sample), and two REAL GC-side windows were found: a
  fresh (never-observed) scheduler's round CREATES `gc/state` between the final LIST and the
  `round_in_flight` read and completes before it (`_gc_promptliteral` red — the Task-8 prose
  containment does not close the completed-round window), and the as-built out-of-round
  `heartbeatLoop` pulses plus the missing scheduler lifecycle-exit recreate control keys in the
  qual window and after promotion (`_gc_asbuilt` red). The fix candidate — scheduled rounds AND
  heartbeat pulses refuse unless the pool lifecycle is `Live`, the refusal the manual GC entry
  points already make — is mechanically validated (`_fix_gclivegate` green). The natural promotion
  is dormant in production until the strong-LIST capability is wired, so these are
  pre-activation findings, not live bugs.
- **`CaDiskLifecycle.tla`** — the rev.7 lifecycle state machine + the as-built
  `SYSTEM CONTENT ADDRESSED FORGET` step order (`Pool::forgetDisk`), concurrent with keeper trips,
  the self-remount thread (whose in-flight attempt may complete a full reclaim after the terminal
  intent is published — intent is checked at step boundaries only), natural terminal promotions,
  the GC scheduler loop, and the `GC STOP`/`GC START` verbs under `lifecycle_mutex`. Proves: FORGET
  ends with the fence latched and a fully-terminal state under all interleavings (the second
  `tripMountLost` is exactly what closes the join-window reclaim race — `_sab_notrip2` reproduces
  Task 10's RED demo mechanically); the farewell is written only after a real drain
  (`_sab_unearnedfarewell` red); `IdentityLost`/`Vanished` are one-way; terminal states imply a
  latched fence; FORGET leaves GC destroyed and unrestartable; and a started FORGET always
  completes (liveness under fairness), including when a racing natural promotion wins
  enterVanished first (first-terminal-wins is by design; `_witness_racederased` shows the race is
  real).

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
