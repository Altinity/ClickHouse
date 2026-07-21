---
description: 'The disciplined engineering methodology that drove CAS MergeTree architecture decisions: TDD, subagent-driven development, TLA+ as a pre-implementation gate, scenario/soak suite as an empirical oracle, and systematic debugging — with the concrete pivots each produced.'
sidebar_label: 'CAS Methodology'
sidebar_position: 2
slug: /superpowers/cas/methodology
title: 'CAS MergeTree — Engineering Methodology'
doc_type: 'guide'
---

# CAS MergeTree — Engineering Methodology {#cas-methodology}

**Status:** DONE (captures practice as of 2026-07-02).

This document explains *how* the CAS MergeTree architecture was developed and why the big pivots happened.
The *what* is in `01-architecture.md`; the protocols are in `03-writer-protocol.md` and `04-gc-protocol.md`.
Sources: night logs 2026-06-04 and 2026-06-05, `UNATTENDED-WORKLOG-2026-06-18-b140-dangle.md`,
`cas-unattended-work-log-2026-06-24.md`, `cas-gc-unattended-execution-log.md`, design reviews
`reports/2026-06-07-ca-spec-review-milovidov*.md`, `reports/2026-06-07-ca-spec-review-distributed*.md`,
and the model currency audit `models/MODEL_CURRENCY_REVIEW_2026-06-22.md`.
(NOTE: most of these source documents were DELETED in the 2026-07-02 docs consolidation — their
content is folded into this doc set; see `CONSOLIDATION-COVERAGE.md` for the mapping. The citations
above are kept as the historical provenance record, not as live links.)

---

## 1. Overview {#overview}

Four interlocking practices governed every phase of development:

1. **Test-driven development (TDD)** — failing tests written before code at every task.
2. **Subagent-driven development** — independent implementer → spec-review → quality-review subagents per
   task; no single-threaded editing.
3. **TLA+ as a pre-implementation gate** — no code task in a behavior-changing phase begins until the
   relevant TLA+ model is green; the gate is not advisory.
4. **Scenario/soak suite as an empirical oracle** — a 24-hour deterministic chaos soak (two replicas,
   shared CA pool, seeded workload + seeded fault injection, quiesced checkpoints with `clickhouse-disks
   ca-fsck` + `ca-gc-dryrun`) as the runtime invariant check.

The design reviews (`reports/2026-06-07-ca-spec-review-milovidov*.md` and
`reports/2026-06-07-ca-spec-review-distributed*.md`) played a fifth role: external adversarial scrutiny
of specifications before TLA+ encoding.

---

## 2. Test-driven development {#tdd}

**Practice.** Every implementation task begins with a failing gtest that exercises the property the task must
establish. The test is committed first (RED state). The task is complete only when all new tests pass and the
existing suite is green. A task-level quality review (separate subagent, fresh context) checks that the
tests are non-trivial (not just passing vacuously) and that the implementation's invariants are preserved.

**Why this mattered operationally.** The gtest suite caught multiple real bugs during the 2026-06-04 night
run:

- `ContentAddressedWriteBuffer` had no `cancelImpl` — on the insert-cancel path its inner hashing/temp-file
  buffers were destroyed "neither finalized nor canceled". The test `02435_rollback_cancelled_queries` made
  this deterministic; the bug had been masked by nondeterminism before.
- The TOCTOU race in `LocalObjectStorage::listObjects`/`tryGetObjectMetadata` (concurrent sidecar deletion
  between dir-listing and per-entry stat) was revealed by the multi-part transaction test batch; the fix is
  universally correct (a real object store snapshots its listing).
- In the 2026-06-18 B140 fix, a code review subagent independently caught that `Build::precommit` emitted
  build-root shards keyed by `build_seq` beyond the table shard fan-out, so `shardsToVisit` had to be
  extended to LIST them. The failing test (T7, `CasBuildRootDangle.SharedBlobSurvivesSourceDrop`) made the
  gap visible before any integration test.

**Oracles as the highest-value tests.** The most durable tests are *safety oracles* — tests that set up a
dangerous interleaving and assert an invariant holds. Examples: `CasBuildRootDangle.SharedBlobSurvivesSourceDrop`
(build-root protects a blob from GC mid-build); `PrematureReclaimCommitFailsClosed` (precommit reclaimed
mid-build → commit ABORTs, never dangles); the S33 scenario card in the soak suite (concurrent GC leader
scenario that exposes the concurrent-leader-leak, expected-FAIL until the fix lands).

---

## 3. Subagent-driven development {#subagent-driven}

**Practice.** Each non-trivial implementation task is handed to a fresh implementer subagent with a
self-contained brief (spec, plan, exact file paths, failing-test list, build/test commands). On completion,
two independent review subagents assess it: a spec-compliance review and a code-quality review. The
orchestrating agent synthesizes the findings, decides on changes-needed vs. approved, and dispatches a
fixer if needed.

**What this bought.** The pattern maintains independent scrutiny throughout. The 2026-06-27 Phase 1a review
(`cas-gc-unattended-execution-log.md`) illustrates the value: the code review subagent found a real
**blocker** — `RunFileReader::loadFooter` parsed untrusted footer length fields before verifying the footer
CRC, giving heap over-read / UB under hardening on corrupt input. The implementer subagent had produced
correct semantics but missed a security boundary. The review prevented the flaw from becoming load-bearing
before it was caught.

**Ground-truth gathering agents.** Before writing an implementation plan, parallel read-only exploration
agents extract verbatim signatures, line numbers, and build constraints from the relevant source files. This
eliminates placeholder-filled plans (plans that say "call the appropriate function") and makes the
subagent's brief self-contained.

**Simplification passes.** Phase B of the 2026-06-05 night run ran three `/simplify` rounds (GC cluster
simplification, architecture reorg, codec-deserializer clarification). Each produced a review-approved
commit. The architecture reorg (Phase B round 2) extracted `RefPayload`, `ObjectIO`, `GcLayout`, and
`ContentAddressedWriteBuffers` into cohesive files; `ContentAddressedTransaction.cpp` shrank from 2505 to
2243 lines. Simplification is not cosmetic — co-located related entities reduce the surface where
invariants can be violated.

---

## 4. TLA+ as a pre-implementation gate {#tla-gate}

**Practice.** Every phase that changes deletion semantics, the GC round protocol, or a safety invariant
must have a TLA+ model that is GREEN before any code task begins. "GREEN" means: every liveness/safety
stage HOLDS; every negative-control (`_sab_*`) VIOLATES its named invariant; no `UNEXPECTED PASS`. A
failure to reproduce the named counterexample in the negative control is as bad as a safety violation —
it means the model does not cover the case.

This is a hard gate, not a documentation exercise. The gate discipline is explicit in the
`cas-gc-unattended-execution-log.md`: *"no code task in a phase begins until that phase's TLA+ suite is
GREEN."*

### 4.1 Counterexample: the incarnation redesign {#tla-incarnation}

**Before (EBR design, 2026-06-07):** the GC used an epoch-based reclamation (EBR) core. The generation was
encoded in the **key** (`blobs/<H>/<g>`), requiring a `404→LIST` degraded read path, durable per-hash
floors, `child_gen` inside tree identity (ancestor rebuilds when a floor moved), and Keeper required for
`safe_epoch` fencing.

**What the reviews found (2026-06-07).** Two parallel adversarial reviews (`reports/2026-06-07-ca-spec-review-milovidov.md`
and `reports/2026-06-07-ca-spec-review-distributed.md`) independently found the D6 write-ahead-intent
mechanism (per-commit per-file persistent Keeper writes to track orphan builds) was unsafe and
over-engineered:

- The distributed-systems review: the D6 "in-degree==0 guard *alone*" was over-stated. In the
  `decide→+`-not-yet-durable window the fold legitimately reads 0; the real protection is `safe_epoch > e_a`
  plus the post-`+` resurrect-on-condemned step. An implementer reading "no rescue, in-degree alone" might
  reclaim the orphan directly in R2, skipping the gate → loss. Separately, the intent key
  `leases/<epoch>/<key>` collided across writers building the same content — owner attribution broke.
- The simplicity review (virtual Milovidov): D6 put `O(files)` **persistent** Keeper writes on every
  commit — re-introducing the data-proportional Keeper traffic that made zero-copy replication painful.
  *"CUT it; crash debris goes to the periodic Retention-guarded sweep."*

D6 was cut entirely. Crash orphans move to a periodic condemn-not-delete reconcile. More fundamentally, the
2026-06-10 incarnation redesign (`specs/2026-06-10-ca-incarnation-store-design.md`) moved the resurrection
marker **into the object body** and deletion precision **into the backend token**, eliminating the generation
in any key. The `404→LIST` path disappeared. A stuck writer can no longer stall pool-wide reclamation —
there is no `safe_epoch` for it to pin.

**The TLA+ gate here:** `CaIncarnationCore.tla` (the canonical core model) carries four invariants —
`INV-NO-DANGLE-COMMITTED`, `INV-BUILDROOT-PROTECTS`, `INV-BUILDROOT-RECLAIM`, `INV-COMMIT-FAILCLOSED` —
and model-checks them exhaustively before the C++ implementation follows the new design.

### 4.2 Counterexample: source-edge set replacing integer refcount {#tla-source-edge}

**Before:** the GC tracked blob liveness via an integer in-degree counter incremented/decremented per
commit. The 2026-06-07 distributed review found a precise race: in the `decide→+`-not-yet-durable window
the fold legitimately reads in-degree 0 for a live blob. If the guard was "in-degree==0 alone" the blob
would be reclaimed, producing a dangle. The fix was not to patch the counter but to replace the model: the
GC log records **source edges** (`+` and `-` deltas keyed by `(kind, hash, source_manifest)`) rather than
bare counters. A fold computes the net in-degree from the edge multiset; the `BlobInDegreeMatchesActiveManifests`
TLA+ invariant asserts the edge multiset is exactly the set of active manifests referencing the blob. With
this structure, the `+`-not-yet-durable problem becomes a pending-fold problem rather than a guard problem:
the fold barrier withholds any `+` until the body is confirmed present, closing the window.

The TLA+ encoding of the edge multiset vs. bare integer is what made the invariant expressible and
checkable at all. The model guided the implementation of `GcDelta` (coalesced `I1`/`I6` `+`-before-ref
under lock, per-shard split, `event_id` dedup-on-fold) and the `foldDeltasIntoGeneration` function.

### 4.3 Counterexample: the registry-removal two-coordinate proof {#tla-registry}

**Before:** the GC discovered namespaces from a persistent `gc/registry`; the fence step CAS-bumped
`fence_round` into every shard of every registered namespace — minting fence-only manifests for absent
shards. Per-round cost was `O(namespaces-ever-created × root_shards)`. The registry grew monotonically
because `dropNamespace` never deregistered.

**Why obvious fixes were rejected.** The design spec `specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md`
enumerates the rejected alternatives:

- Writer deregisters at `dropNamespace`: unsafe because the removal events (the `-1` in-degree edges) are
  the only carrier of blob reachability updates; if the namespace vanishes from discovery before GC folds
  that window past the fence, the `-1`s are lost → permanent blob leak.
- GC hard-deregisters when "empty + settled": an empty-but-live namespace (table with no inserts) is
  indistinguishable from a dropped one by emptiness alone. Inferring "empty ⇒ retire" would deregister a
  live table between `CREATE` and the first `INSERT`.
- Both of the above plus shard-object deletion on the current path-keyed cursor: deleting and later
  recreating an object at the stable path `cas/refs/<ns>/<shard>` is an ABA hazard. The fold cursor is
  keyed by path and filters journal events by `transition_version`; a recreated shard resets
  `shard_version` to 0 and an old sealed cursor silently skips the new incarnation's events → lost edges →
  dangle or leak.

**What the TLA+ model proved.** `CaGcShardIncarnationCore.tla` encodes two orthogonal coordinates:
`incarnation` (a durable, monotone, never-reused tag stamped into the ref-shard object at creation) and GC
`round` (the pool-global clock). The model checks `INV_NO_DANGLING` and `INV_NO_ORPHAN_EDGE` across
724,944 distinct states. The three negative controls each break the invariant they target:
`SabotageNewbornNoFloor` (round self-floor is irreducible), `SabotagePathKeyedCursor` (incarnation is
irreducible — ABA), `SabotageDeleteBeforeFold` (fold-before-reclaim ordering).

**The one-vs-two question.** The TLA+ run answered directly: two coordinates are necessary. Neither the
incarnation alone nor the GC round alone is sufficient — the model shows a counterexample for each
one-coordinate variant, and the two-coordinate design is clean. This answered a design question that had
been deferred because the alternatives looked superficially viable.

With the theorem proven, the registry is deleted entirely (`gc/registry`, `RootsRegistry`, `CasRootsRegistry`,
`ensureRegistered`, `registered_cache`, the registry-fence sub-step). Discovery migrates to `LIST(cas/refs/)`.

### 4.4 The B140-dangle TLA+ gate {#tla-b140}

The B140 dangle (a blob GC-deleted while still referenced by a committed part) was pinned live in the soak
via `system.content_addressed_log` (B170). The approved fix was a build-root/precommit redesign: protection
becomes *reachability from a durable build root*, not a revocable per-blob hint. The fragile
`cas_owner`/`protectedByLiveBuild` per-candidate machinery was scheduled for deletion.

The TLA+ model (`models/CaBuildRootPrecommit.tla`) used a 2×2 flag matrix (BuildRoot ∧ FailClosedCommit):

- Buggy (no build-root alone): `INV_NO_DANGLE_COMMITTED` violated in 481 states — the counterexample
  matches the soak dangle exactly (write→adopt→BuildDie→GcDelete→Commit).
- Fixed (build-root ∧ fail-closed): clean exhaustive 24,205 states.
- **Sharp finding:** build-root *alone* still dangles (ordering window); fail-closed *alone* is clean only
  vacuously. Both halves are independently necessary and jointly sufficient.

The model validated the design before a line of C++ was written. The C++ implementation followed the proof
(`UNATTENDED-WORKLOG-2026-06-18-b140-dangle.md`, T4→T10).

### 4.5 Attempt-scoped generations and the retired-set hazard {#tla-attempt}

The concurrent-leader leak (soak scenario S33) involved deposed GC leaders leaving stale retired-set
artifacts visible to writers. An initial deviation from the spec proposed attempt-scoping only the four
`gc/gen/<gen>/…` write-once artifacts and leaving `retired`/`outcomes` under existing `(round, fence_seq)`
keys, calling stale debris "bounded sweep, minor."

The user review rejected this: `retired` is a **writer-facing publish-gate input** — `RetireView::refresh`
LISTs the whole `gcRetiredPrefix()` and writers consult it on the commit path. A stale retired set written
by a deposed leader under its own `(round, fence_seq)` survives in that LIST and **influences live writers**
— directly violating *"no unadopted artifact may ever influence a decision."* There was also no existing
sweep for stale `gc/retired/`+`gc/outcomes/` artifacts.

The spec was followed exactly: all decision-bearing round artifacts (including `retired` and `outcomes`)
move under `gc/gen/<g>/attempt/<a>/…`. This also simplifies cleanup — the `gc/gen/<g>/` retention prune
reclaims retired/outcomes automatically, no separate sweep.

---

## 5. Scenario/soak suite as an empirical oracle {#soak-oracle}

**Structure.** The soak harness (`utils/ca-soak/`) runs two `ReplicatedMergeTree` replicas sharing one CA
pool on RustFS (or a conformant S3 backend). A seeded, deterministic Python ledger drives insert /
merge-pressure / mutation / TTL-delete / truncate workers. A seeded chaos injector fires container
kill/restart/pause faults at scheduled windows. At quiesced checkpoints (writers paused, replication
queues drained, merges idle, GC at a fixpoint), the harness asserts:

- SQL results match an independent model oracle on **both** replicas.
- `clickhouse-disks ca-fsck` reports `dangling=0` (INV-NO-LOSS: every reachable object exists).
- `ca-gc-dryrun` cross-checks: GC would delete only genuinely-unreachable objects.
- `unreachable=N (M-F debris)` is a known-acceptable residual (abandoned builds — Full GC M-F, deferred).

The `ProfileEvent` budgets (e.g. S3 operation counts, `S3WriteRequestsErrors`, `gate_revalidate`,
`gate_resurrect`) provide a quantitative check that the protocol is operating within expected bounds.

**What the soak revealed that tests did not.** Several bugs were first manifested in soak runs, not in
gtests:

- **B140 dangle (2026-06-18):** `reuseBlob` (the adopt operation) transferred the blob hash but not the
  `cas_owner` protection, so the adopted blob's protection remained bound to the retired byte-writer. GC
  reclaimed it under a still-in-flight adopter. The soak's `system.content_addressed_log` (B170 event
  log) pinned the precise dangle event with its token and timing.
- **Group-A resurrection-cap exhaustion (2026-06-05):** in an aggressive test GC cadence, the fixed
  iteration cap of 8 in `resolveAndResurrectGeneration` was exceeded, producing `CORRUPTED_DATA` failures
  in ordinary insert/mutation tests. The cap-8 choice was justified by the original design's generation
  bound but not by the test's GC rate. The soak-like full-suite run surfaced 33 failures from this
  single root cause; the fix (cap 8→256) was a one-line change.
- **rustfs 412 vs. data loss (2026-06-20):** the soak reported 1.6M `S3WriteRequestsErrors` and 94
  `fsck dangling` after a chaos window, appearing to be a Phase-6 regression. Forensic analysis of the
  event log showed all 922,772 GC deletes had `indeg_at_recheck=0` and zero deleted-while-referenced.
  The 1.6M errors were ~1.57M `PreconditionFailed` (412) — by-design CAS conditional-write contention on
  root-shard manifests, not storage failures. The 94 dangling was a transient read-unavailability
  false-positive (429/503 load shedding during chaos); the 180s settle budget was too short under the
  write load. The soak distinguished Phase-6-is-correct from harness-needs-improvement.

**Harness fragility as its own finding.** The 2026-06-19 VFS soak run needed four restarts to reach the
chaos stage, each exposing a harness limitation rather than a product bug: TTL-band checkpoint
over-strictness (fixed with bounded wait-out + re-quiesce), merge-aware quiescence (a 620s large-merge
over S3 was misidentified as a hang), setup DROP timeout under accumulated data (fixed with fresh cluster
on each run), and compose startup ordering. Fixing harness fragilities is itself a design output —
the harness's behavior under load directly shapes what the soak can prove.

**`ca-fsck`/`ca-gc-dryrun` as an independent read path.** The `ca-fsck` command opens the CA disk in a
**read-only mode** that does not require the server to be running. It recomputes the full reachability
graph independently (traversing roots → shard manifests → refs → trees → blobs), compares against the
physical pool content, and reports `dangling` / `orphan` / `unreachable` counts. The `ca-gc-dryrun`
previews what the next GC round would delete without executing the deletions. Together they constitute an
independent check on both INV-NO-LOSS and GC-deletes-only-unreachable — the two safety properties the
protocol is built around.

---

## 6. Systematic debugging {#systematic-debugging}

**Root-cause-first.** Every incident follows a read-only diagnosis phase before any fix is written.
The diagnosis names the exact failing invariant, the exact code path, and the exact interleaving that
triggers it. Fixes never pre-empt a diagnosis.

**Same-class defect hunts.** After a class of bug is fixed, a read-only hunt searches for structurally
equivalent sites in the codebase. The 2026-06-05 hardening pass (H1–H5) illustrates: after the B85
read-path generation-resolution fix and the B87 `moveFile`-on-attach-rollback fix, five same-class
sites were found:

- H1: `ContentAddressedMetadataStorage::getLastModified` called `resolvePartGenKeyForRead` (unchecked)
  at four sites — a stale `active` hint could 404 and wedge `clearOldTemporaryDirectories` (DROP).
- H2/H3: two sites threw `LOGICAL_ERROR` for benign concurrent-DROP/DETACH races where
  `FILE_DOESNT_EXIST` (recoverable, matching plain-disk semantics) was correct.
- H4: `replaceFile` lacked the `is_part_dir` directory-delegation that `moveFile` had just received —
  defense-in-depth for a future regression.
- H5: `ContentAddressedGC::listLivePartIds` did a LIST-then-`readSmallObject` without swallowing a
  concurrently-dropped ref — an aborted GC round under DROP churn.

The same-class hunt is disciplined: it uses grep + code reading with explicit scope (CA path only; non-CA
sites noted but out of scope). Findings are triaged by severity (server-abort vs. recoverable vs.
defense-in-depth) and fixed in a single reviewed commit before the next integration test.

**Confirming "not a regression" before fixing.** The 2026-06-19 VFS path-mapping run found 68 unique
failures in the full suite. Before writing any fix, a baseline determination ran the two failing test
families (`transactions`, `text-index`) against the pre-refactor binary on the same host. Both families
failed identically on the baseline — they were pre-existing CA gaps (B182, B183), not regressions. The
refactor was declared regression-clean without any code change.

---

## 7. Design review as methodology {#design-review}

Both the simplicity/performance review (virtual A. Milovidov persona) and the distributed-systems
adversarial review (Lamport happens-before analysis) were run against the 2026-06-07 Merkle-store spec
**before any TLA+ encoding and before any implementation**.

The reviews produced five findings that changed the architecture:

1. **D6 cut entirely** (both reviews independently): write-ahead lease intents re-introduced
   data-proportional Keeper traffic. Removed; crash orphans moved to periodic condemn-not-delete reconcile.
2. **Intent key collision** (distributed review): `leases/<epoch>/<key>` collided across writers building
   identical content. Moot after D6 was cut, but confirmed D6 was irreparable as specified.
3. **In-degree-alone over-stated** (distributed review): the precise reclaim window where the fold
   reads 0 for a live blob. Led directly to the source-edge multiset design.
4. **`O(1)` memory for reconcile** (simplicity review, second pass): the §4.5 streaming merge-sort
   (LIST ⋈ sorted stream, never materializing reachability) was adopted as the only framing that does not
   OOM at 10¹¹ objects. The review gave explicit criteria: "one condemner, not two writers."
5. **Keeper epoch cache: sole-writer rule must be normative** (distributed review): the cache is correct
   only if nothing else ever writes the epoch znode; the condition must be a stated invariant (§3.2, §6.2)
   with an assertion in the code, not just prose.

The model currency audit (`models/MODEL_CURRENCY_REVIEW_2026-06-22.md`) performed a retrospective check:
after several months of development, do the TLA+ models still correspond to the code? The audit found no
old-design cruft (no EBR-epoch / `resurrect-by-GET` / `cas_owner` / `reuseBlob` / seal-TTL) in any live
model. The real drift was around the B171 precommit-reachability mechanism (which replaced per-candidate
watermark guards) and three models that were now stale against the shipped code — `CaBuildWatermark.tla`,
`CaBuildWatermarkNum.tla`, and `CaResurrectLiveness.tla`. These were not old-design garbage but modeled a
protection path the code had either removed or never shipped. The audit documented the dispositions (mark
stale; repurpose the watermark floor lemma for precommit-ref reclaim liveness) rather than deleting the
models, preserving the proof history.

---

## 8. Per-practice status summary {#status-summary}

| Practice | Status | Notes |
|---|---|---|
| TDD | **DONE** — enforced throughout | Gtest sweep is the merge gate for every behavior-changing commit |
| Subagent-driven development | **DONE** — enforced throughout | Two-review (spec + quality) pattern per task |
| TLA+ gate (core models) | **DONE** — `CaIncarnationCore`, `CaBuildRootPrecommit`, `CaGcLeaseCore`, `CaGcRootLocalPartManifestCore`, `CaGcShardIncarnationCore` | See `06-tla-models.md` for full index |
| TLA+ gate (D1 registry removal) | **DONE** — `CaGcShardIncarnationCore` green (724,944 states, two-coordinate proof) | Implementation landed 2026-07-01/02 (`gc/registry`/`RootsRegistry` deleted, discovery via `LIST(cas/refs/)`, shard incarnation stamped); status note updated 2026-07-03 |
| Scenario suite | **DONE** (S01–S35 cards) | S33 concurrent-leader scenario now PASSES as a real regression guard (attempt-scoped generation landed 2026-06-28); see `08-testing-and-soak.md §5.1` |
| 24h deterministic soak | **DESIRABLE** — blocked on conformant backend | rustfs 412 vs. write-error confound needs harness-side fsck retry; currently 4-hour runs |
| `ca-fsck`/`ca-gc-dryrun` | **DONE** — shipped, used in every soak checkpoint | |
| Model currency review | **DONE** — 2026-06-22 audit; dispositions recorded | Three stale models identified; `CaGcCore.tla` HISTORICAL |
| Full GC (M-F) | **TODO** — deferred; soak's `unreachable=N` residual is this | Spec slot reserved in `CaIncarnationCore.tla` |
