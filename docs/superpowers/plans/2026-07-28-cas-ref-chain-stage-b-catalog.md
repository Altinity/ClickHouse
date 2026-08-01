# CAS ref catalog + incarnations — Stage B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the namespace universe authoritative and rebirth-proof: the `ref_catalog`
(INV-3) with ref-layer-scoped incarnations, the namespace lifecycles (§3), universe-from-catalog
for GC/recovery/fsck/sweep, plus the actionable register items (R2/R3/R5, R1-spec) and the TLA
follow-up debt — closing Stage A's named universe residual.

**Architecture:** Spec `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md`
INV-3 + §2 read-side contract + §3 lifecycles + the register
`docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md`. Prerequisite: Stage A
complete with `STAGE A: PASS` in `docs/superpowers/cas/2026-07-28-stage-a-RESULTS.md` — Stage B
re-keys the layer Stage A built and swaps discovery to the catalog without touching Stage A's
arithmetic.

**AMENDED 2026-07-29** by the authoritative user directive
`docs/superpowers/specs/2026-07-29-cas-stage-b-namespace-life-amendments.md`, which is
AUTHORITATIVE FOR SCOPE AND INTENT over both this plan and the spec sections it touches.
Four design changes and three implementation improvements: namespace identity generalizes from
refs to refs AND namespace files (`NamespaceLifeId`, replacing the planned `RefNamespaceId`);
namespace files are re-keyed under `<ns>/<inc>/_files/<relative-name>`; recovery becomes fully
LIST-independent; `_ckpt` gains an explicit `O(1)`-size invariant plus corruption-on-conflict
join rules; and three extractions/renames (`prepareRefChunk`, `chooseRecoveryGrounding`,
`tryPublishSnapshotAndAdvanceCheckpointOnce`). **Spec supersession, explicit:** INV-3's clause
"verbatim FILES stay unqualified and keep today's `_cleanup` gate — their pre-existing
rebirth-aliasing hazard is register item R1, not this spec" is SUPERSEDED — the file layer IS
qualified here, and register R1 is consequently re-scoped (Task 9). Every other spec sentence
stands. The amendment adds Tasks 1b, 1c, 4b, 4c, 5b, 6b; the pre-existing integer numbering is
FROZEN (the SDD ledger, the Task-1 review rounds and Stage-A cross-references all cite it), so
inserted work carries a letter suffix meaning "runs after N", exactly as Task 7b already does.

**EXTENDED 2026-07-30** by a second authoritative user directive,
`docs/superpowers/specs/2026-07-30-cas-gc-destructive-baseline-directive.md`, which charters the
GC-focused tail of this plan: delete probe A (Task 7a), enable catalog-proven destructive GC against
an unchanged gate formula (Task 7b), run a SEQUENTIAL-baseline destructive soak with a measured cost
inventory (Task 11 Steps 3c/3d), and write the successor GC performance report (Task 12). Its
sequencing rationale is binding and is recorded as Constraint 18: probe-A removal and destruction
enablement are separate changes so that a performance effect can be told apart from a possible
correctness regression, and no `MultiDelete` or delete concurrency enters this round at all — the
soak's purpose is the honest baseline those optimizations will later be measured against.

**Tech Stack:** identical to Stage A (same tree, same gates, same soak harness).

## Global Constraints {#global-constraints}

**COMMENT POLICY (user, 2026-07-31) — binds every task below, and one measured baseline to beat.**

The goal is code readable and understandable **without** comments: a comment never substitutes for a clear
name, a tight interface, or a type that makes the wrong thing unrepresentable. When something needs a long
explanation to be safe to touch, the code is what changes.

- **No comment may cite a plan, spec, BACKLOG entry, review round, finding ID or task number.** Those
  artefacts are branch-local and get deleted; a citation to one becomes a pointer no reader can resolve.
  **The REASON is durable, the provenance is not — keep the reason, drop the citation.**
- **Comments must** give the reason for a non-obvious decision, explain a complex algorithm or a non-local
  invariant the code cannot state itself, and document modules and interfaces in **headers** so code
  intelligence surfaces the contract at the call site.
- **Keep them short.** Long prose desynchronises from code faster than short prose.
- **A rule that becomes an executing check deletes the rule's prose.** Otherwise the mechanism is paid for
  and the refund never collected — which is what happened when `FsckReport::clean` became a `static_assert`
  and the paragraph explaining the old rule stayed.

**Baseline measured 2026-07-31, so progress is checkable rather than asserted:** the CAS subsystem is 50169
lines with **16919 comment lines (33%)**; the longest contiguous comment blocks are **83, 65, 63, 61, 57**;
and there are roughly **850 branch-local references** in code comments (`Task N` ~490, `spec §` ~293, plus
`R1x`, `docs/superpowers`, `BACKLOG`, `review C3`, `codex finding`). Every task that opens a file applies the
policy to what it touches; the subsystem-wide sweep is Task 14.


Constraints 1-11 of the Stage A plan
(`2026-07-28-cas-ref-chain-stage-a-streams.md {#global-constraints}`) apply VERBATIM to every
task here. Additional Stage-B constraints:

12. **[AMENDED 2026-07-31]** The catalog entry's incarnation is the opaque physical `life_id` for
    THE REF LAYER AND NAMESPACE FILES. The logical `RootNamespace` remains in the catalog and in the
    in-memory `NamespaceLifeId`, but it is absent from life-owned object keys. Immutable stream
    objects live at `cas/ns/stream/<life_id>/{_log,_snap,_cleanup}` (`_cleanup` exists only until
    Task 5 deletes the class); point/path-addressed state lives at
    `cas/ns/state/<life_id>/{_ckpt,_files}`. The split is by access pattern, not by ownership: both
    prefixes are under the one logical `cas/ns/` ownership tree, while only `stream/` is enumerated
    on every fold round. A new birth of the same logical name gets a new `life_id`, so a LIST-hidden
    old file remains structurally unreachable. Explicitly UNCHANGED, both directions:
    manifests keep `(namespace, mount-epoch, build-sequence)` identity and their existing
    globally unique build identities; LOOSE MOUNTPOINT OBJECTS stay outside namespace ownership
    and unqualified (they are why the plain-object component survives — Constraint 17); and the
    current DIRECT-OBJECT implementation of namespace files stays as it is — this amendment
    changes object identity, never file persistence.
13. Catalog admission refuses loudly; removal is NEVER refused (spec INV-3).
14. Format bump B (`generation=5`, Task 4) and Task 4b are already-landed development
    intermediates. Task 4d owns the recreate-only layout cut at `generation=6`: it removes the
    logical name from physical life keys and introduces `stream/` + `state/`. Task 5 owns the
    distinct recreate-only wire cut at `generation=7`: it replaces the fold seal with `ref_lives`
    and deletes `_cleanup` plus its cleanup-item state. There is no migration or dual reader: an
    older generation fails when the pool opens and the pool is recreated. Thus Task 5 refuses a
    generation-6 pool rather than discovering its obsolete same-generation grammar later. Regenerate
    goldens at each honest format cut: layout/generation goldens for Task 4d, then the fold-seal and
    cleanup goldens for Task 5.
15. **`_ckpt` stays `O(1)` [directive §4, verbatim]:** "`_ckpt` must remain a fixed-size product
    of scalar monotone facts. Its encoded size must be `O(1)` in the number of refs, files,
    transactions and writer epochs. Do not add maps, collections or cardinality-growing fields.
    Such state belongs in a separate immutable object or ledger." Binding on every task, not just
    Task 4c.
16. **Deduplication performance constraint [directive, verbatim]:** "Do not move namespace files
    into the ref log in this work. `MergeTreeDeduplicationLog` rotates files frequently on the
    insert path because the CA disk does not support append. Adding catalog reads, blob uploads,
    ref-log appends or whole-directory manifest rewrites would directly affect insert latency.
    Incarnation qualification must preserve the current namespace-file operation profile: no
    catalog request per file operation; no ref-log append; no blob upload; no folder-manifest
    rewrite; unchanged direct-object backend request counts."
17. **Out of scope [directive, verbatim]:** "Moving namespace files into the ref log. Folder-state
    manifests or a `_raw_files` object family. Changes to `MergeTreeDeduplicationLog`. Merging
    `Poisoned` into wedge. Refactoring `RefApplyState` in this series. Removing the entire
    plain-object component; loose mountpoint objects still require it." And the clarification that
    goes with it, verbatim: "`Poisoned` is not equivalent to wedge: it means a durable transaction
    may be missing from the cached view, so it must continue to block snapshot publication and
    trigger re-recovery." A task that finds itself wanting any of these raises instead of landing.
18. **[ADDED 2026-07-30 by the GC destructive-baseline directive] The destructive round stays
    SEQUENTIAL for this whole series:** "Пока не добавлять MultiDelete и parallel deletes: сначала
    получить честный baseline и понять реальные затраты". No `MultiDelete`/batch-delete API, no
    parallel delete fan-out, no delete-side concurrency anywhere in Tasks 7a/7b/11/12 — the point of
    the round is an honest per-operation cost baseline, and a batched or parallel implementation
    measured against no baseline is unfalsifiable. Optimizations are the NEXT round's work and are
    justified BY these numbers. The directive's sequencing rationale is binding for the same reason:
    probe-A removal lands as its own change BEFORE destruction is enabled, "так легче отличить
    performance effect от возможной correctness-регрессии" — two effects in one commit are two
    effects nobody can separate afterwards.

## What the lane restatement changed for the tasks below {#restatement-impact}

Measured 2026-07-30, after `bb4dd513118` landed, by checking every symbol these tasks name against
the tree rather than assuming: **no later task is stale in its target.** `trySnapshotPublishOnce`,
`runRecoveryWalkOnce`, `recoverRefTable`, `recoverRefTableDetailed`, `hint_log_ids`,
`publishCkptContribution`, `observedNamespaceCleanupMarker`, `sweepStalePrecommitsNow` and
`resolveWedgeOnce` all still exist, and all four models Task 10's TLA debt names still exist (that
debt is about the GC models, not the lane). The restatement rewrote the machinery INSIDE
`CasRefLedger`; these tasks address the catalog, the lifecycles, the re-keying, recovery grounding,
the read side, decommission, probe A, the destruction flip and the gates — none of which was written
against the encodings that died. Four specifics do carry over:

1. **These no longer exist. Do not cite them, and treat a citation of them anywhere as stale text:**
   `RefApplyState` and its `applyStateForTest`, `armApplyPending`/`clearApplyPending`/
   `poisonApplyState`, the durable floor (`durableFloorCovers`, `noteDurableIdNotApplied`), the type
   `RefAppendWedge` (now `RefAppendAttempt`), and the THIRD post-durable install region (two remain).
   A stale `refCkptKey` doc reference survived four tasks before Task 1c swept it — that is the
   failure mode this list exists to prevent.
2. **Recovery is now a hard fence, and Task 5b builds on that rather than around it.** `NeedsRecovery`
   blocks writes AND certification until replay completes, `requireRecovery` is how a site enters it,
   and its only exit is a COMPLETE recovery install — asserted, not assumed (the sabotage that makes
   recovery incomplete turns the caught-up invariant red). Read
   `docs/superpowers/specs/2026-07-30-cas-ref-lane-state-machine.md` before re-deriving any recovery
   entry semantics. The LIST-independence work itself is unchanged: the genesis fallback and the
   entirely-LIST-driven second entry point are both still there to remove.
3. **Certification is `Ready`-only, and Task 6b must preserve that.** The confirm gate refuses on one
   state-agnostic comparison evaluated BEFORE row equality, and snapshot publication carries the same
   gate — so splitting or renaming the publish path must keep the gate on both halves.
4. **Use the lane's own test observers rather than inventing new ones:** `laneStateForTest` and
   `refLaneWedgedForTest` exist and are widely used; a later task's test that needs to see lane state
   should read them.

## Review policy: non-code findings are batched, not looped {#prose-findings-batched}

USER DIRECTIVE 2026-07-30. A finding whose entire content is non-executing — a comment, doc text, a
report or commit-message claim — is appended to `docs/superpowers/cas/deferred-docs-fixes.md` and
executed in ONE later pass. It does NOT enter a per-task fix round. Every review dispatch from here on
says so, so reviewers still report prose findings and grade them, but nothing waits on them.

The reason is in that file: Task 1c spent three consecutive rounds on comment accuracy with zero
defects in code or tests, and each round wrote the next round's finding.

**MANDATORY REVIEW TRIGGER — the sanitizer-lane abort class.** A throw-expectation on a
`LOGICAL_ERROR` site outside `#ifndef DEBUG_OR_SANITIZER_BUILD` is a **review defect**, not a style
nit: constructing a `LOGICAL_ERROR` exception ABORTS under `DEBUG_OR_SANITIZER_BUILD`
(`Exception.cpp`, `handle_error_code`), so the test cannot pass there — and the abort HIDES every
test after it in the binary. Five recurrences in one week (`CasWiring`, `PartFolderAccess` twice,
`DenyGuard`, then `CasRefNamespaceId.ZeroIncarnationIsUnconstructible` on sha `6fa8a0c9316`).

Every review of a diff that adds or moves a test runs:
`grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"` over the touched test files, and
checks each hit against its throw site's ACTUAL error code — `CORRUPTED_DATA` expectations are fine.
And when one test in a file is split, **the whole file gets swept**, because the abort means a second
offender would have been invisible behind the first. The fix is a death-test split that keeps the
`Cas` prefix (the gate filter is `Cas*:CA*`) and is verified on BOTH a sanitizer build and a release
build — the `#else` arm is where a wrong `EXPECT_DEATH` matcher hides.

**Classify before filing.** Of the six minors deferred from Task 1c's review, only three were prose;
the rest were executing defects and are now steps of Task 7. A finding can also be filed as wording
while its body names a consequence — split those, and send the behaviour to `BACKLOG.md`.

## Task overview {#task-overview}

| # | Task | Source | Depends on |
|---|---|---|---|
| 0 | Stage-A gate preflight | — | — |
| 1 | `RefNamespaceId` type; namespace-only ref overloads deleted — **EXECUTED** | §2 r9-3 | 0 |
| 1b | Pure `prepareRefChunk` extracted out of `commitRefChunk` | directive impl-1 | 0 |
| 1c | `RefNamespaceId` → `NamespaceLifeId`: file keys covered, no `RootNamespace` conversion | directive §1 | 1,1b |
| 2 | Catalog object: format, states, capacity admission | INV-3 | 0 |
| 3 | Creation lifecycle + three-site recheck completion | §3 | 1c,2 |
| 4 | Re-key ref layer under `<ns>/<inc>/`; universe from catalog; format bump B | INV-3/§5 | 1c,2,3 |
| 4b | Re-key namespace files under `<ns>/<inc>/_files/`; rebirth waits for nothing | directive §2 | 4 |
| 4c | `_ckpt` strengthened: `O(1)` invariant + corruption-on-conflict join | directive §4 | 4 |
| 4d | Opaque physical `life_id`; split `cas/ns/stream` from `cas/ns/state` | layout amendment | 4b,4c |
| 5 | Removal lifecycle: one ref-life row, proved direct deletion, perpetual janitor | §3 + directive §2 | 3,4d |
| 5b | `chooseRecoveryGrounding` + LIST-independent recovery + the LIST audit | directive §3/impl-2 | 4c,5 |
| 6 | Read-side contract: handles, pre-delete revalidation, namespace-file read/write closure | §2 + directive | 4d,5 |
| 6b | `trySnapshotPublishOnce` → `tryPublishSnapshotAndAdvanceCheckpointOnce` | directive impl-3 | 4c,6 |
| 7 | R5 decommission duties | register R5 | 4d,5 |
| 7a | DELETE probe A — the second full stream LIST and everything that serves it | GC directive §1 | 4d,5b |
| 7b | Destruction enablement: `UniversePolicy::kDefault` → authoritative | staging contract + GC directive §2 | 4d,5,6,7,7a,10f |
| 8 | R2+R3: writer duty queue + orphan-blob nomination (one coherent change) + model extensions | register R2/R3, §9 | 5 |
| 9 | R1 closure note (verbatim-file rebirth aliasing) — doc only, RE-SCOPED | register R1 | 4d,6 |
| 10 | TLA debt: seven independent review units | phase follow-ups | mixed: 10a after 5b; 10f after 5; remaining units independent |
| 11 | Stage B gates: battery + churn/rebirth/decommission soak + the sequential-baseline destructive soak + verdict | §9 + GC directive §3 | all |
| 12 | GC performance research on the destructive baseline + the successor report | GC directive deliverable | 11 |
| 13 | Post-Stage-B: split the two 4000-line files, goldens FIRST | refactor candidates | 12 |

Task 10 has seven independently scheduled units: Task 10a follows Task 5b, Task 10f follows Task 5
and is a hard predecessor of Task 7b, while the other five units are independent of the code chain;
its sub-tasks obey the file-ownership split in `{#parallel-execution-lanes}`. Task 9 is doc-only but is no longer
schedule-free: the re-key must exist before it can record where each R1 sub-hazard went. Task 11's
soak REQUIRES Task 7b (destruction enabled) — a
soak with destruction still suppressed does not exercise Stage B's claims. Task 12 requires Task 11's
soak ARTIFACTS, not merely its verdict.

**Recommended execution order** (a topological order of the column above; the directive's
§Execution commit list is honored in its own relative order):

`1b → 1c → 2 → 3 → 4 → 4b → 4c → 4d → 5 → 7 → 5b → 6 → 6b → 10f → 7a → 7b`, with
Task 8 after Task 5, Task 9 after Task 6, Task 10a after Task 5b and the other Task-10 units in
their stated lanes, then Task 11 and finally Task 12.

**The GC tail (Tasks 7a → 7b → 11's destructive soak → 12) is a SEQUENCE, not a set**, and the
2026-07-30 directive's rationale is why: probe-A removal is a performance change, destruction
enablement is a correctness change, and running them together makes the soak unable to attribute
either. Constraint 18 keeps the implementation sequential so the soak produces a baseline the next
round's `MultiDelete`/concurrency work can be measured against.

Directive commit → task: (2) pure preparation → **1b**; (3) general namespace-life identity →
**1c**; (4) ref and file re-keying → **4** (refs, already planned) + **4b** (files), with their
physical generation-5 placement superseded by **4d** while their life-aware APIs survive; (5)
strengthened `_ckpt` → **4c**; (6) recovery grounding → **5b**; (7) cleanup/read-side closure →
**5** (cleanup half) + **6** (read/write half); (8) snapshot naming or split → **6b**.

Two scheduling notes, both deliberate:
- Task 7 (decommission) is pulled AHEAD of Task 5b. Task 5b makes "`Removing` with a missing
  `_ckpt`" a window that recovery REFUSES to ground; the owners of that window are the removal
  driver's self-resume (Task 5) for a live root and Task 7's `_ckpt`-absent branch for a dead one.
  Landing Task 7 first means the window never lacks an owner. Nothing in the dependency column
  forces Task 6 before Task 7, so this costs nothing.
- The directive's items (6) recovery grounding and (7) cleanup/read-side closure are mutually
  independent; the order above keeps the directive's relative order anyway.

## Parallel execution lanes {#parallel-execution-lanes}

Time pressure does not make two writers in `CasGc.cpp` or `CasRefLedger.cpp` independent. The critical
path remains `4d → 5 → (7 ∥ 5b) → 10f → 7a → 7b → 11`; commits enter the
integration branch in dependency order. Work may run concurrently only from the same committed base,
with explicit file ownership and sequential integration. A shared dirty worktree is not a parallelism
mechanism.

**Wave P0 — start beside Task 4d:**
- One owner implements Task 4d's core (`CasLayout`, physical parsers, catalog reverse index and mount
  safety). Do not split those interfaces across workers.
- After that owner freezes the two prefix helpers and expected literal grammar, a mechanical lane may
  update layout goldens, test literals, integration prefix constants, soak classifiers and prose. It
  owns no production `.cpp` file and rebases nothing; its commit is applied after the core layout
  commit and any failures are fixed in the owning layer rather than hidden with compatibility aliases.
- Task 10b/10c/10d/10e/10g are model-tree work and may proceed in a separate worktree. Task 10a waits
  for Task 5b's recovery shape. Task 10f and edits to `CaRefCatalogCore`, `CaRefDeltaIntakeCore` or
  `CaRefNsCleanupStaleLeaderCore` wait for Task 5's model commit because Task 5 owns those exact files;
  Task 10f integrates before Task 7b.
- Task 11's report skeleton, suite inventory, artifact directories and an exploratory tidy build may
  start now. Final gates and the authoritative tidy verdict still rerun after Task 7b.

**Wave P1 — after Task 4d is integrated:**
- Freeze `RefLifeFoldState`, `RefCoverage`, `RefCleanupEvidence` and the `buildRefWalkPlan` signature
  first. A codec lane may then own only `CasFoldSealFormat`, `CasGcShardPlan.h` and their focused tests;
  a model lane owns only the three TLA modules/configurations; the core lane owns `CasGc.cpp`, catalog
  lifecycle code and the removal integration test. The codec commit lands before the core commit, while
  the model commit remains the code gate. No two lanes edit one production file.
- Task 4d-specific tool/test cleanup that was not needed for its gate may continue only in files Task 5
  does not name. Task 8 begins only after Task 5 and is a poor parallel candidate: it overlaps both
  `CasGc.cpp` and `CasRefLedger.cpp`, so defer it unless it has a dedicated worktree and an owner willing
  to integrate after the critical path rather than merge speculative conflicts into it.

**Wave P2 — after Task 5 is integrated:**
- Task 7 (decommission) and Task 5b (recovery grounding) may run concurrently from the same Task-5
  commit. Task 7 owns `CasDecommission` plus its new tests; Task 5b owns `CasRefLedger`,
  `CasRefProtocol`, recovery tests and the LIST residual. Shared catalog/lifecycle interfaces are frozen
  by Task 5; neither lane changes them. They may integrate in either order: direct deletion removed the
  former dead-root finalization dependency.
- Task 6 may replace Task 5b as Task 7's parallel partner, but it may not be a third production lane:
  both Task 6 and Task 5b rewrite `CasRefLedger`/`CasRefProtocol` recovery and handle seams. The critical
  schedule chooses Task 5b. If elapsed time matters, give Task 6's namespace-file tests and
  request-profile audit to a test-only lane while Task 5b owns production code.

**Wave P3 — after Task 5b and Task 7 are integrated:**
- Task 7a is primarily `CasGc`/settings/soak cleanup and may run beside Task 6's reader/namespace-file
  implementation. `CasPool.h` is the one expected collision; assign it to Task 6 and let Task 7a delete
  its obsolete setting in a small follow-up after Task 6 lands.
- Task 9's documentation-only closure may run as soon as Task 6's final diff is known. Task 6b waits for
  Task 6 and remains single-owner in `CasRefLedger`.
- Gate harness preparation and long non-authoritative baseline captures may run continuously; no result
  taken before Task 7b substitutes for Task 11's final battery or destructive soak.

This schedule gives useful parallelism without stacked PRs: every externally reviewed change still
targets `master`; local worktrees produce ordinary commits that are integrated onto this one feature
branch in dependency order.

---

### Task 0: Stage-A gate preflight {#task-0}

- [ ] **Step 1:** `grep -n "STAGE A: PASS" docs/superpowers/cas/2026-07-28-stage-a-RESULTS.md`
  (NOTE 2026-07-29 evening: the verdict flipped to PASS at commit `3f7b35c7ce1`; the earlier
  PENDING note is history. The grep now matches — on the bare verdict line and in the header
  prose. "Exactly one match" is amended to: the BARE line `STAGE A: PASS` exists — use
  `grep -nx "STAGE A: PASS"` for the exact-line form.) — else BLOCKED.
- [ ] **Step 2:** Re-run the CA gtest gate filter (Stage A Task 0's exact command); record
  baseline counts in the report. Baselines at Stage A's final head `d4ddc736949`:
  release 1566/243 suites, ASan 1570/256 suites (RESULTS `{#final-head-gates}` addendum; the
  earlier `25ce1d3531a`/1564/1568 figures predate the final-review I1-fix merge — Task 0's
  first run caught this line being stale, 2026-07-29).
  No commit.
- [ ] **Step 3 (carried from the Stage A final review, rec 5):** verify the two capstone
  tests are still RED-WHEN-FIXED sentinels and the residual they guard is still open:
  `gtest_cas_list_liar_end_to_end.cpp` carries two tests (regions `:511`, `:561`) DELIBERATELY
  written to go red when the `recoverRefTable` LIST residual dies, with the fix instruction in
  the failure message; the residual itself is `[RECOVER-REF-TABLE-LIST-RESIDUAL]` in
  `docs/superpowers/cas/BACKLOG.md` and is a 7b PRECONDITION. If those tests have gone red,
  the residual was fixed — update the BACKLOG entry and adapt the tests per their own failure
  text BEFORE any Stage B task touches recovery. This coupling is deliberate: the flip must
  refuse until the residual dies.

### Task 1: `RefNamespaceId` — dropping the incarnation becomes unrepresentable {#task-1}

**EXECUTED 2026-07-29** in lane-g at `34d607e72ae` (+ fix `67dd2666e75`, review round 1 in
flight), i.e. BEFORE the amendment arrived — so it shipped the type as `RefNamespaceId`, over ref
keys only. This record is HISTORY and is not re-opened: the type is not renamed here, the ref-key
migration is not redone, and the two commits stand. **Task 1c is the amendment's continuation of
this task** and owns, in one change: the rename `RefNamespaceId` → `NamespaceLifeId` (directive §1
generalizes the identity to namespace files, so a name saying "ref" becomes wrong), the widening
of coverage to `_files` keys, the extension of this task's concept-negative battery to the
namespace-file overloads, and the new "does not implicitly convert to `RootNamespace`"
requirement. Everything below describes what LANDED; every later task reads `NamespaceLifeId`
wherever it says `RefNamespaceId`.

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/RefNamespaceId.h`
  (EXECUTED 2026-07-29 as `Primitives/CasRefNamespaceId.h` — controller-accepted deviation: the
  type is pure vocabulary beside `RootNamespace`/`RefTxnId`, and `Formats/CasLayout.h` including
  `Pool/` would have been the tree's first violation of the documented include direction
  `Primitives → Formats → … → Pool`. Type name unchanged; later tasks reference the TYPE.)
- Modify: `.../ContentAddressed/Formats/CasLayout.h` — the `Layout` class [path and type per
  codex finding 18] — every ref-layer prefix/key/parser member helper
  (`refsNamespacePrefix`, `refLogKey`, `refSnapshotKey`, `refCkptKey` from Stage A, and the
  parsers `parseRefObjectKey`/`parseRefLogKey`) changes its namespace parameter to the new type;
  the `RootNamespace`-only overloads are DELETED (not deprecated — Constraint 4)
- Modify: every caller (the compiler enumerates them — that is the point of r9-3)
- Create: `src/Disks/tests/gtest_cas_ref_namespace_id.cpp`

**Interfaces:**
- Produces (every later task consumes):
```cpp
struct RefNamespaceId
{
    RootNamespace ns;
    UInt128 incarnation = 0;   /// 0 is INVALID — never a wildcard; constructors reject it

    /// The ONLY constructors: from a catalog entry (discovery paths — recovery/fold/fsck/sweep)
    /// or from a live handle (readers). No default construction, no from-namespace-only.
};
```
- Key grammar (INV-3): `<ns>/<inc>/{_log,_snap,_ckpt}` with `<inc>` fixed-width lowercase hex;
  parsers REFUSE legacy-shaped keys (`CORRUPTED_DATA` naming the key — behind Task 4's format
  bump every legacy shape is a corruption, not a compat case).

- [ ] **Step 1: Failing tests**: key round-trip through every helper; parser refuses a
  legacy-shaped key, a zero incarnation, a malformed hex; a compile-time assertion test that
  the namespace-only overload set no longer exists — via a GENUINELY TEMPLATED concept so the
  negative check is a substitution failure, not a hard error [codex r2 finding 18]:
```cpp
template <class L>
concept HasNamespaceOnlyRefLogKey =
    requires(const L & l, const RootNamespace & ns, const RefTxnId & id) { l.refLogKey(ns, id); };
static_assert(!HasNamespaceOnlyRefLogKey<Layout>);
```
  — the "cannot compile" half of spec §9 r9-5 #3 (repeat the concept per deleted helper).
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement type + re-plumb until the tree compiles —
  mechanical, compiler-driven; in Stage B Task 1 the incarnation VALUE everywhere is a
  placeholder constant from the not-yet-landed catalog: introduce
  `RefNamespaceId::stageATransition()` (a named constant incarnation) used ONLY by this task's
  plumbing; it is DELETED by Task 6 (after the reader replumb), with the tree-wide zero-grep
  gate in Task 6's steps [codex r3 finding 1].
- [ ] **Step 4:** Full CA gate green. **Step 5: Commit**
  `ca: ref — RefNamespaceId{ns, incarnation}; namespace-only key helpers deleted`.

### MEASUREMENT 2026-07-30 — Task 1 CAN be merged; the silent surface is one line, inside a conflict {#task-1-merge-measured}

Measured rather than assumed, by the same method that exposed Task 1b's trap. `git merge-tree
--write-tree 67dd2666e75 cas-gc-rebuild` (lane-g as of Task 1 + its fix, against the restated master)
yields tree `e04675baaea` with **two conflicted files**: `Pool/CasRefLedger.cpp` (ONE hunk, markers at
2716/2721/2726) and `tests/gtest_cas_ref_install_safety.cpp`.

**The decisive question was the key API**, since Task 1 DELETED the namespace-only overloads while
master still has 317 call sites in that form. The answer: the restatement introduced exactly **ONE**
namespace-only call site — `prepared_attempt.key = layout.refLogKey(ns, id);` — and it sits INSIDE the
`CasRefLedger.cpp` conflict hunk (merged-tree line 2724, immediately below lane-g's migrated
`prepared_wedge.key = layout.refLogKey(RefNamespaceId::stageATransition(ns), id);` at 2719). It also
REMOVED two — the `prepared_wedge` key line it replaced, and one test's `refsNamespacePrefix(ns)`
fault-substring. Everything else auto-merges because the two changes are DISJOINT: Task 1 migrated key
builders, the restatement rewrote the lane's state machinery.

**Verified per file in the merged tree**: every file that calls the five ref-key helpers also carries
`RefNamespaceId`/`stageATransition`/`ns_id` at least as often as it calls them — with exactly one
exception, `Formats/CasRefCkptFormat.h:13`, which is the STALE DOC COMMENT already queued for Task 1c,
not code. So **no namespace-only key call silently lands** in a tree whose `Layout` no longer offers
that overload. This is the opposite of Task 1b, where the auto-merged part could not compile.

**What this measurement does NOT prove, stated so nobody over-reads it:** `merge-tree` is textual. It
says no key-API breakage lands silently; it does not say the merged code is semantically right, and it
is not a build. The real review surface after resolving is the two conflict hunks plus the three files
BOTH sides edited (`gtest_cas_ref_install_safety.cpp` 191 restated lines,
`gtest_cas_ref_wedge_every_attempt.cpp` 101, `gtest_cas_ref_ckpt.cpp` 22) — and Task 1's fix-round
absorb helper in `CasGc.cpp` wants a re-read against the restated lane, since its premise was the
old wedge model.

**EXECUTED 2026-07-30: merged as `9f9514a90f1`, build clean (`NINJA_EXIT=0`), gate 1577/244 all
passed.** Both hunks resolved as predicted — `CasRefLedger.cpp` took master's `RefAppendAttempt` with
the key on `stageATransition`, and `gtest_cas_ref_install_safety.cpp` took master's `NeedsRecovery`
assertion while NOT resurrecting lane-g's test for the install region the restatement deleted. The
silent-survivor grep (`RefApplyState`, `applyStateForTest`, `armApplyPending`, `clearApplyPending`,
`poisonApplyState`, `prepared_wedge`, the TYPE `RefAppendWedge`) returned ZERO. Arithmetic verified
empirically: 1567/243 + 8 (new `CasRefNamespaceId` suite) + 2 (fix-round tests) = 1577/244.

**Consequence for sequencing:** Task 1 lands by MERGE with two hand-resolved hunks; Task 1b is REDONE
on top of it (see below), and the redo then gets the `RefNamespaceId` key API for free instead of
re-extracting against `refLogKey(ns, id)` and making Task 1c migrate the same keys twice.

### ✅ Task 1b REDO COMPLETE 2026-07-30 {#task-1b-redo-done}

Four commits on `cas-gc-rebuild`: `3e228272dd3` (equivalence fences, goldens authored BEFORE the
extraction — verified structurally, not on the report's word), `8d536022eab` (the extraction, the
birth-`_ckpt`/seal reorder, the pure TU), `ec3a73656ea` and `eeb194af24b` (prose). Gate **1585 tests /
245 suites**, all passed; build and style clean. Reviewed twice with **zero defects found in the code
or the tests** both times; the five rounds it did cost were entirely about comment prose, and the loop
was ended by DELETING sentences rather than rewriting them — every false claim across those rounds was
an attribution to a location the author was not reading at the time. Two behavioural deltas are
disclosed in-source: the reorder (a seal refusal now lands before the birth `_ckpt` is durable) and the
fault-class narrowing (an allocation failure in the moved statements now takes the same handler an
apply failure already took, with the tenure continuing). The fault-class delta is deliberately NOT
pinned by a test: both of its endpoints are already pinned, only the routing of a hypothetical OOM at
two statements is not, and the cheap seam would have fired at the extraction BOUNDARY where the
existing catch already covers the callee — so it could not have witnessed the delta at all. That trade
holds only while the disclosure is true, which is why its factual errors were fixed as Important.

### DECISION 2026-07-30 — Task 1b is REDONE on top of the restatement, NOT merged {#task-1b-redo}

The lane restatement (`bb4dd513118`) landed on `cas-gc-rebuild` after Task 1b was built in `lane-g`,
and a read of both trees settles what happens next. **The restatement does NOT supersede the
extraction**: master has no `prepareRefChunk`, no `PreparedRefChunk`, and no other pure preparation
helper — `commitRefChunk` is still one ~550-line monolith mixing preparation, persistence and
settlement. What the restatement rewrote is the half AFTER preparation (`RefAppendWedge` →
`RefAppendAttempt`, a pre-send arming block that moves the prepared attempt into the runtime BEFORE
the first send, `armApplyPending`/`clearApplyPending`/`poisonApplyState`/`RefApplyState` all replaced
by `requireRecovery` + `lane_state`, and the third post-durable install region deleted). The
preparation sequence the task targets survives almost textually — so the task is still worth doing,
with a changed return type and a changed neighbour.

**Why REDO and not merge, and it is the SILENT part of the merge that decides it.** `git merge-tree`
reports only TWO conflicted files, which reads like comfort and is the trap: what AUTO-merges is
broken. The merged header keeps `struct PreparedRefChunk { … RefAppendWedge prepared_wedge; … }`
while the type above it is now `RefAppendAttempt` — a header that cannot compile. The merged `.cpp`
keeps BOTH lane-g's `prepareRefChunk` producing a wedge AND master's inline attempt construction. And
the surviving lane-g body calls `layout.refLogKey(RefNamespaceId::stageATransition(ns), id)` where
master's calls `layout.refLogKey(ns, id)` — two incompatible key APIs in one file. A clean-looking
merge would reintroduce the very apply-marker/wedge model the restatement deliberately deleted.

**The redo, four steps, small and auditable** (the extracted body is 61 lines):
1. Re-extract the pure preparation returning a `RefAppendAttempt` plus the birth `_ckpt`
   contribution, leaving master's arming block in `commitRefChunk` BETWEEN preparation and the first
   send.
2. Re-land the birth-`_ckpt`/seal reorder and its rationale (master still publishes the `_ckpt`
   before sealing).
3. Rewrite the two round-2 comments against RESTATED facts — **two** probe regions, not three; fix
   the tail-call premise (the true reason is "no remainder is left: every batch item is either
   already completed or in this chunk's survivor set", NOT "`chunk_survivors` is the entire batch",
   which `survivors.clear()` refutes); fix the check-enabled-lane claim (`!NDEBUG` only — CI
   sanitizer lanes are `RelWithDebInfo` via `CMAKE_BUILD_TYPE=None`, so the guard is a no-op there
   and only `Debug`/tidy lanes have it live); and fix the `EXPECT_GT` FAILURE MESSAGE, which still
   states verbatim the causal claim the comment above it retracts.
4. Re-derive the preparation-test goldens (the sealed-body size and digest asserted in
   `gtest_cas_ref_chunk_preparation.cpp` are wedge-era and the attempt/key changes may invalidate
   them).

**Prerequisite decision, still open:** lane-g also holds Task 1 (`RefNamespaceId`, 287 call sites)
and the GC ref-key refusal, and NEITHER is in master (`CasLayout.h:130` still takes
`RootNamespace`). Sequence Task 1's landing FIRST if the redo wants the `RefNamespaceId` key API;
otherwise the re-extraction uses master's `refLogKey(ns, id)` and Task 1 lands after. That call is
the controller's and is not made here.

### Task 1b: pure `prepareRefChunk` out of `commitRefChunk` {#task-1b}

[Directive implementation improvement 1 = amendment commit 2. FIRST of the amendment series and
ordered BEFORE the rename (Task 1c) on the directive's own sequence: the extraction is easier to
review on the pre-rename surface, and the rename that follows is then mechanical over the extracted
function too. It depends on nothing but Task 0 — it is pure, backend-free protocol arithmetic.]

**Files:**
- Modify: `.../Pool/CasRefLedger.h` — declare `prepareRefChunk` beside `commitRefChunk` (`:818`);
  the `_ckpt`-contribution comment block (`:862-865`) gains the prepared-birth reference
- Modify: `.../Pool/CasRefLedger.cpp` — `commitRefChunk` (`:2613`): the pre-durable region moves
  out. Today's locals, so the extraction is a MOVE and not a rewrite: `candidate` (`:2712`),
  `candidate_base_id` (`:2713`), `id` (`:2714`), `admitted_fence_generation` (`:2715`),
  `last_epoch_seal` (`:2716`) — all populated under `state_mutex` at `:2717-2724`; `chunk_txn`
  (`:2731`, `RefLogTxn{ns.string(), id, chunk_ops, chainLinkFor(id, last_epoch_seal)}`); the
  in-memory `applyRefLogTxn(*candidate, chunk_txn)` (`:2734`); `prepared_wedge` with its key and
  sealed bytes (`:2789-2795` — already built BEFORE the `PUT` per spec §A1 site 3, which is exactly
  why this region is extractable); `armApplyPending` (`:2809`) stays in `commitRefChunk` (it mutates
  the runtime)
- Create: `src/Disks/tests/gtest_cas_ref_chunk_preparation.cpp`

**Interfaces:**
- Produces (the directive's shape, with today's local names as the members):
```cpp
/// Everything `commitRefChunk` DECIDES before its first durable effect. Backend-free by
/// construction: no request, no clock, no lock, no mutation of `RefTableRuntime`.
struct PreparedRefChunk
{
    RefTableState candidate;                    /// the candidate table state
    RefTxnId candidate_base_id;                 /// greatest-applied at preparation time
    RefLogTxn chunk_txn;                        /// includes the chain link
    RefAppendWedge prepared_wedge;              /// COMPLETE: txn id, canonical key, sealed bytes,
                                                /// admitted fence generation
    std::optional<RefCkpt> birth_contribution;  /// set iff this chunk is the namespace birth
};

PreparedRefChunk prepareRefChunk(const RefTableState & state, const RefTxnId & id,
                                 const std::optional<RefTxnId> & chain_link,
                                 std::span<const RefOp> ops, uint64_t admitted_generation);
```
- PRESERVED, verbatim from the directive — each one is a review checkpoint: "backend request
  counts; all fault and ambiguity semantics; the existing wedge protocol; allocation-free
  post-durable install regions; existing post-durable fault seams". And: "Do not broadly rewrite
  settlement in the same change" — settlement stays where it is, this task only lifts preparation
  out from in front of it.
- **The boundary is the FIRST DURABLE EFFECT, and `commitRefChunk` has TWO of them depending on
  chunk shape — getting this wrong makes the extraction silently unsound:** an ordinary chunk's
  first durable effect is the ref-log
  `putIfAbsentControlled(prepared_wedge.key, prepared_wedge.bytes, …)` at `:2818`, but a
  `NamespaceBirth` chunk durably writes `_ckpt` FIRST, at `:2755-2776`
  (`publishCkptContribution(ns, RefCkpt{.life_epoch = id.writer_epoch, …})` at `:2760-2763` —
  INV-4's first `_ckpt` writer), AHEAD of the wedge and the `PUT`. Therefore `prepareRefChunk` is
  called before `:2755`, and `birth_contribution` is the PREPARED VALUE ONLY — publishing it stays
  in `commitRefChunk`. A "pure" function that writes `_ckpt` would be a lie, and moving that write
  earlier would change the fault semantics the directive says to preserve. Everything
  `prepareRefChunk` returns must be computable with the backend disconnected; the test TU proves
  that by not having one.

- [ ] **Step 1: Failing tests** in `gtest_cas_ref_chunk_preparation.cpp`. The TU includes NO backend
  header and instantiates no backend — that is the mechanical proof of "backend-free", checkable by
  a reviewer reading the include list:
  `PreparedKeyAndSealedBytesAreCanonical` (round-trip: the prepared key parses back to
  `(life, id)`, the sealed bytes decode back to `chunk_txn`);
  `CandidateBaseIdIsGreatestApplied`;
  `ChainLinkRequiredExactlyOnSequenceOneOfNonGenesisEpoch` (INV-2's grammar, now exhaustively
  testable: the cross product of {genesis, non-genesis} × {sequence 1, sequence >1} × {seal present,
  absent} — the point of the extraction is that this needs no backend);
  `BirthContributionSetOnlyForNamespaceBirth` (and its `life_epoch` equals `id.writer_epoch`);
  `PreparedWedgeIsCompleteBeforeAnyDurableEffect` (every wedge field populated — an `Unresolved` arm
  must have nothing left to build).
- [ ] **Step 1b: the equivalence guard** (this is what makes an extraction safe to review):
  `CommitRefChunkDurableBytesUnchangedByExtraction` — for a fixed table of inputs, the key and the
  sealed body that reach the backend are byte-identical to the pre-extraction tree (capture the
  golden values from HEAD BEFORE extracting, paste them into the test as literals);
  `AppendRequestCountUnchanged` — the append lane's op journal per commit is unchanged
  (Constraint 8's +0-requests rule, asserted rather than assumed);
  `PostDurableInstallRegionStaysAllocationFree` — the `DENY_ALLOCATIONS_IN_SCOPE` regions
  (`CasRefLedger.h:683`) still cover the `Committed` install and the wedge move, and the extraction
  moved no allocation into them.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Extract (move, do not rewrite). **Step 4:** Full CA gate
  green vs the Task-0 baseline. **Step 5: Commit**
  `ca: ref — extract pure prepareRefChunk from commitRefChunk`.

### Task 1c: `RefNamespaceId` → `NamespaceLifeId`, over refs AND namespace files {#task-1c}

[Directive design change 1 = amendment commit 3. The continuation of Task 1: same idea, wider
surface, plus the one requirement Task 1 did not have (no implicit conversion to `RootNamespace`).]

**Files:**
- Rename: `.../Primitives/CasRefNamespaceId.h` → `.../Primitives/CasNamespaceLifeId.h`; the type
  `RefNamespaceId` → `NamespaceLifeId` (git mv + a mechanical, compiler-enumerated rename across
  the 37 files / 287 call sites Task 1 touched, plus this task's new file surface)
- Rename: `src/Disks/tests/gtest_cas_ref_namespace_id.cpp` →
  `src/Disks/tests/gtest_cas_namespace_life_id.cpp`
- Modify: `.../Formats/CasLayout.h` — `namespaceFileKey(const RootNamespace &, const String &)`
  (`:231`, returns `prefix + "/roots/" + ns.string() + "/_files/" + file_name`) and
  `namespaceFilesPrefix(const RootNamespace &)` (`:244`) take `NamespaceLifeId`; the grammar becomes
  `roots/<ns>/<inc>/_files/<relative-name>`; the `RootNamespace`-only overloads are DELETED
  (Constraint 4); the header's key-shape documentation block (`:73`) is updated. `mountpointObjectKey`
  (`:281`) is NOT touched — loose mountpoint objects stay unqualified (Constraint 12)
- Modify: `.../Pool/CasPool.cpp` — `listNamespaces` carries TWO parse hazards in ONE function, both
  discharged here because the function is being edited anyway (leaving a known fsck-killer in a
  function you are already touching is worse than a marginally wider task; split the commit if the
  diff grows). Found by the Task-1c scouting pass and by Task 1's re-review:
  (a) the `_files` recovery SPLITS THE STRING on `"/_files/"` (`:~1449`) and takes everything before
  it as the namespace — once files are life-keyed that silently yields `<ns>/<inc>` as the namespace.
  Teach it the segment via `parseNamespaceFileKey` and make it fail closed, exactly as Task 1 did for
  ref keys. §required-tests' "rejection of legacy unqualified ref and `_files` keys" lands here;
  (b) the REF-key parse (`:1421`) is UNGUARDED, so the life-less-key refusal Task 1 introduced escapes
  into `CasFsck`'s loop HEADERS (`CasFsck.cpp:548`, `:846`, `:1027`) — outside the per-namespace
  "RECORD AND CONTINUE, NEVER WEDGE" try at `:560`, and `runFsck` rethrows everything but
  `TIMEOUT_EXCEEDED` — so ONE malformed key makes fsck produce NO report at all, including about the
  healthy namespaces it never reached. That is the forensic tool an operator reaches for after seeing
  the new GC anomaly, so it dying first is the wrong failure order (Task-1 re-review IMPORTANT-A,
  tracked return-item). Same exposure from `removeRecursive` (`ContentAddressedTransaction.cpp:1063`)
  and decommission (`CasDecommission.cpp:119`). SHAPE, decided 2026-07-30 after the implementer
  surfaced the consumer asymmetry: absorb AT THE ENUMERATION (Task 1's shape) rather than hoisting
  into fsck's headers, because `listNamespaces` has FOUR independent consumers — `removeRecursive`,
  the metadata-storage scope enumeration, `ca-fsck` and `ca-decommission` — so guarding the producer
  fixes all four while guarding fsck fixes one. **But the producer must SURFACE the skip as DATA,
  never swallow it, and it must not decide policy**: a silently short namespace list is WORSE than a
  loud abort for `ca-decommission`, which RETIRES SLOTS — it could conclude "drained" over a
  namespace the listing omitted, which is data loss, not a usability wrinkle. So the enumeration
  returns the skipped keys (count + keys, or a per-key error list) and each consumer disposes of them
  per its own stakes: **fsck RECORDS-AND-CONTINUES** (it must still report the healthy namespaces —
  that is the whole point), **decommission REFUSES fail-close** (an incomplete universe cannot
  license retiring a slot; same stance as the drained-root refusal in BACKLOG
  {#ckpt-failed-birth-debris}), and the remaining two get the disposition their own call sites argue
  for. If surfacing turns out not to be expressible at the producer, hoisting wins — say so with the
  code reason rather than forcing the preferred shape
- Modify (SAME AUDIT, placed by the placement sweep 2026-07-30 — do NOT split it across tasks, since
  splitting is how the first half got missed): `Gc/CasGc.cpp`'s `rebuildBaseline` gen-0 health check
  (`:3756` vicinity) hands per-life keys straight to `groupRefKeys` with no catch, so a NESTED-shape
  corrupt key (`<ns>/<inc>/x/_log/<id>.zst`) refuses at `namespaceLifeOf` and kills the recovery
  COMMAND — "the recovery command must not be taken out by the damage it exists to recover from",
  which is the new test's own argument turned on its author. Task 1 newly exposed it (before the
  re-key such a key parsed as an opaque deeper namespace and did not throw). Narrow reachability
  (needs `snap_generation == 0` plus a foreign nested key), so a guard plus one test row, not a
  redesign. This is Task-1 re-review MINOR-B, previously adjudicated to "the Task 6 family" and never
  written into any task.
- **RE-OPENED RULING, and the reason is that its evidence base changed after it was made:** Task 1's
  review minor 2 ruled the `listNamespaces` DDL-path `CORRUPTED_DATA` "acceptable — loud and bounded".
  That ruling predates IMPORTANT-A, which showed the SAME escape kills fsck entirely. The DDL consumer
  is `removeRecursive`, so one stray key makes a **DROP fail — and the drop is what would have removed
  the offending namespace.** That is the self-perpetuating shape (the operation that would clear the
  obstacle is the one the obstacle blocks) which upgraded IMPORTANT-A from usability to blocking, and
  it applies here for the same reason. Since this task must give ALL FOUR consumers a disposition
  anyway, decide the DDL consumer's here on the new evidence rather than inheriting a ruling made
  without it. Flagged by the implementer against their own earlier under-rating of the fsck half.
- Create (in `CasLayout.h`/`.cpp`): `parseNamespaceFileKey` — the `_files` mirror of
  `parseRefObjectKey`, returning `(NamespaceLifeId, relative-name)`; the janitor (Task 5) needs it to
  classify keys from its bounded pool-wide `rootsPrefix` scan. **Historical generation-5 shape:**
  Task 4d later changes the returned identity to physical `life_id` and moves the scan to `cas/ns/`;
  do not copy this executed Task-1c signature into new code.
- Modify: `.../Pool/CasPlainObjects.h`/`.cpp` — `putNamespaceFile` (`.h:48`/`.cpp:93`),
  `getNamespaceFile` (`.cpp:98`), `listNamespaceFiles` (`.h:57`/`.cpp:103`), `removeNamespaceFile`
  (`.h:61`/`.cpp:127`) all take `NamespaceLifeId`; `.../Pool/CasPool.h` forwarders (`:543-549`)
- Modify: the callers — `ContentAddressedMetadataStorage.cpp` (`:1325`, `:1498-1501`, `:1647-1648`,
  `:1695-1696`, `:1846`), `ContentAddressedTransaction.cpp` (`:822`, `:1112-1114`, `:1235-1237`,
  `:1410-1411`, `:1586`), `Gc/CasGc.cpp` (`:3002`, `:3137`) — re-typed here, RE-SEMANTICISED later
  (Task 4b gives real incarnations; Task 5 changes what removal deletes). Where no life handle
  exists yet, `NamespaceLifeId::stageATransition()` carries it, exactly as Task 1 did for refs, and
  Task 6 Step 1b's tree-wide zero-grep gate is what finally forbids it
- Modify: `src/Disks/tests/gtest_cas_layout.cpp` (`:69-70` key/prefix expectations; `:106-112`
  relative-name rejections must survive the re-typing verbatim), `gtest_cas_gc_fold.cpp` (`:552`),
  `gtest_cas_ref_gc.cpp` (`:830`)

**Interfaces:**
```cpp
struct NamespaceLifeId   /// was `RefNamespaceId`; renamed because it now qualifies namespace FILES
{                        /// as well as ref objects, so a name saying "ref" is a lie
    RootNamespace ns;
    UInt128 incarnation;   /// 0 is INVALID — never a wildcard; constructors reject it

    /// The ONLY constructors, both surviving from Task 1: `fromCatalogEntry(ns, incarnation)`
    /// (discovery paths) and the transitional `stageATransition(ns)` that Task 6 deletes; the real
    /// ctor stays private. No default construction.
    /// NO implicit conversion to `RootNamespace` and no `RootNamespace` constructible from it:
    /// the call sites that legitimately need the bare name (manifest identities, loose mountpoint
    /// objects, the all-lives LIST prefix) say `.ns` and are visible in review.
};
```
- **Honest finding: the no-implicit-conversion requirement is ALREADY met structurally** — the type
  declares no conversion operator, and no `RootNamespace` constructor takes a `NamespaceLifeId`
  (`Primitives/CasTypes.h`), so nothing interconverts in either direction today; only explicit
  `.ns` crosses. This task therefore FENCES the property with compile-time assertions rather than
  changing behaviour, and the report says so instead of claiming a fix that was not needed. The
  fence is the point: without it, a later convenience conversion lands unnoticed.
- Key grammar after this task: refs `<ns>/<inc>/{_log,_snap,_ckpt,_cleanup}`, files
  `<ns>/<inc>/_files/<relative-name>`, `<inc>` canonical fixed-width lowercase hex; both parsers
  REFUSE legacy-shaped keys with `CORRUPTED_DATA` naming the key (Constraint 14 — behind bump B a
  legacy shape is corruption, never a compat case).

- [ ] **Step 1: Failing tests** — compile-time half, extending the EXISTING battery in
  `TEST(CasNamespaceLifeId, NamespaceOnlyKeyHelpersDoNotExist)` (Task 1 shipped it as
  `TEST(CasRefNamespaceId, …)`, concepts at `:57-90`, assertions at `:249-262`, five paired helpers:
  `refsNamespacePrefix`, `refLogKey`, `refSnapshotKey`, `refCleanupMarkerKey`, `refCkptKey`).
  **The Task-1 pattern extends to the file-key overloads exactly as-is: one negative per deleted
  namespace-only overload, each PAIRED with a positive so a typo'd concept cannot pass vacuously**
  (a misspelled member name makes the negative trivially true — the positive is what proves the
  concept is looking at something real). Keep the tree's existing `HasNamespaceOnly…`/
  `HasIncarnation…` naming:
```cpp
template <class L>
concept HasNamespaceOnlyNamespaceFileKey =
    requires(const L & l, const RootNamespace & ns, const String & n) { l.namespaceFileKey(ns, n); };
template <class L>
concept HasIncarnationNamespaceFileKey =
    requires(const L & l, const NamespaceLifeId & life, const String & n) { l.namespaceFileKey(life, n); };
static_assert(!HasNamespaceOnlyNamespaceFileKey<Layout>);
static_assert(HasIncarnationNamespaceFileKey<Layout>);   /// the pairing that kills vacuous typos
```
  — repeated for `namespaceFilesPrefix`, plus the three type-level assertions that fence §1's
  remaining requirements:
```cpp
static_assert(!std::convertible_to<NamespaceLifeId, RootNamespace>);
static_assert(!std::constructible_from<RootNamespace, NamespaceLifeId>);
static_assert(!std::is_default_constructible_v<NamespaceLifeId>);
```
  and TWO deliberate POSITIVE out-of-scope fences — `mountpointObjectKey(const String &)`
  (`CasLayout.h:281`) and `manifestNamespacePrefix(const RootNamespace &)` (`CasLayout.h:215`) still
  exist un-life-scoped. If either of those asserts ever fails, someone qualified loose mountpoint
  objects or manifests, which Constraint 12 forbids — the amendment is explicit that both keep
  today's identity.
- [ ] **Step 1b: Failing tests** — runtime half: `namespaceFileKey` round-trips through
  `parseNamespaceFileKey` for a flat and a NESTED relative name (`deduplication_logs/…`);
  `parseNamespaceFileKey` refuses a legacy `roots/<ns>/_files/x` key, a zero incarnation, malformed
  or upper-case hex — each `CORRUPTED_DATA` naming the key; the `:106-112` relative-name rejections
  (empty, leading/trailing `/`, `//`, `..`) still hold under the new signature.
- [ ] **Step 0 (ORDERING-CRITICAL — must precede any key change):** capture the pre-change
  namespace-file REQUEST-COUNT GOLDENS from the current tree — per-key backend request counts for
  rewrite, append (`ContentAddressedTransaction.cpp:822`), remove, and one dedup-log ROTATION
  sequence — and paste them into the guard test as LITERALS, so the guard fails if incarnation
  qualification perturbs the profile. Re-deriving expectations after the change would measure the
  change against itself; this is the same hard ordering constraint Task 1b's equivalence fences hit.
  Task 4b's Constraint-16 gate EXTENDS this file rather than re-deriving its numbers.
- [ ] **Step 1c: Failing tests — `listNamespaces`' two hazards.** A life-keyed `_files` key is
  recovered with the namespace ONLY (never `<ns>/<inc>`); a legacy `roots/<ns>/_files/x` key is
  REFUSED with the key named; and a life-less REF key under `cas/refs/` does not abort the
  enumeration — with `ca-fsck` still producing a report that names the healthy namespaces (the
  IMPORTANT-A regression). If the fsck fixture is too heavy for this TU, assert the enumeration's own
  outcome and state the substitution in the report rather than dropping the claim.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Rename + widen until the tree compiles. **Step 4:** Full
  CA gate green (expect the Task-1 delta: renamed suites, +new file-key cases). **Step 5: Commit**
  `ca: ref — NamespaceLifeId over refs and namespace files; namespace-only file overloads deleted`.

### Task 2: Catalog object — format, states, capacity {#task-2}

**Files:**
- Create: `.../Formats/CasRefCatalogFormat.h` / `.cpp` (+ registry entry in
  `Formats/CasFormat.cpp`: Control/Strict; object cap 256 MiB, line cap 4 KiB)
- Create: `.../Pool/CasRefCatalog.h` / `.cpp`
- Create: `src/Disks/tests/gtest_cas_ref_catalog.cpp`

**Interfaces:**
- Consumes: `casPut` token-CAS; Stage A's `publishCkpt`/fence discipline.
- Produces:
```cpp
enum class NsState : uint8_t { Creating = 1, Live = 2, Removing = 3 };

struct CatalogEntry
{
    RootNamespace ns;
    NsState state = NsState::Creating;
    UInt128 incarnation = 0;              /// minted at Creating, random 128-bit
    /// Required iff state == Creating (strict grammar): the creator's fence identity.
    std::optional<CreatorFence> creator;  /// {server_root_id, writer_epoch, fence_generation}
};

struct RefCatalog                          /// one object, key `cas/ref_catalog`, token-CAS
{
    std::vector<CatalogEntry> entries;     /// canonical order: by ns bytes; duplicates illegal
};
```
- [Cross-reference only, nothing in this task changes] `CatalogEntry` is one of the exactly two
  legal sources of a `NamespaceLifeId` (Task 1c; the other is an already-held live handle) — the
  discovery paths construct it from `{ns, incarnation}` of an entry they just read.
- Capacity admission (INV-3, both halves) — TWO INDEPENDENT predicates [codex r2 finding 9],
  computed by Stage A Task 8's shared byte-arithmetic helper (its constants are the source of
  truth — do not re-derive):
  (1) `encoded_catalog_bytes <= catalog_object_cap` — the catalog's own size;
  (2) `fold_fixed_bytes + Σ worst_case_entry_reservation <= fold_seal_object_cap` — the
  fold-seal reservation for every admitted entry.
  Namespace names get a byte bound (`kMaxNamespaceBytes = 512`). EQUALITY IS ACCEPTED on both
  predicates; cap + 1 refuses. Admission failure is loud (`LIMIT_EXCEEDED` naming the
  namespace and WHICH predicate); removal is never refused (Constraint 13).

- [ ] **Step 1: Failing tests**: codec round-trip all three states; strict rejections
  (duplicate ns; `creator` present on `Live`; `creator` absent on `Creating`; zero incarnation;
  non-canonical order; name over byte bound); token-CAS create/update/conflict-retry over
  InMemoryBackend; PER-PREDICATE boundary tests [codex r3 finding 9]: predicate (1)
  `encoded_catalog_bytes <= catalog_object_cap` accepted at EXACT equality and refused at
  cap + 1 while predicate (2) holds slack, and predicate (2)
  `fold_fixed_bytes + Σ reservation <= fold_seal_object_cap` accepted at equality / refused at
  cap + 1 while (1) holds slack — each refusal message asserts the NAMED failing predicate;
  removal (`Live→Removing` CAS) succeeds at a full catalog (never refused).
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Full CA gate green.
- [ ] **Step 5: Commit** `ca: ref — ref_catalog object: states, incarnations, additive capacity admission`.

### Task 3: Creation lifecycle + the three-site recheck completion {#task-3}

**Files:**
- Modify: `.../Pool/CasRefCatalog.cpp` (+the lifecycle driver), the namespace-creation call
  path (today's `NamespaceBirth` writer — locate from `RefOpKind::NamespaceBirth` usages)
- Create: `src/Disks/tests/gtest_cas_ns_creation_lifecycle.cpp`

**PLACED HERE BY THE PLACEMENT SWEEP (2026-07-30), previously an unplaced deferral:**
`[CKPT-FAILED-BIRTH-DEBRIS]` (BACKLOG `{#ckpt-failed-birth-debris}`) names a "STAGE-B LIFECYCLE
OWNER" and its own one-line closure — a lifecycle-owned `_ckpt` delete for NEVER-BORN namespaces —
but no task executed it; the plan's only mention was a precedent citation in Task 1c, and a citation
is not an executor. This task owns the birth sequence and is the only place that knows a birth did
not complete, so the closure lands here: when a birth is abandoned or fails after its `_ckpt` was
published, the lifecycle deletes that `_ckpt`. Effect if it stays unplaced is operator-visible:
permanent debris that makes a drained server root REFUSE decommission (`claimOwnerOrThrow` →
`CORRUPTED_DATA`). NOTE the partial overlap so nobody assumes it is already closed: Task 1b's
extraction NARROWED this — a birth refused by the structural grammar no longer leaves an inert
`_ckpt`, because the seal now runs before the publish — but a birth whose ref-log `PUT` fails AFTER
the `_ckpt` published still leaks, and that is the case this task must close. Test it as its own row
in `gtest_cas_ns_creation_lifecycle.cpp`.

**Interfaces:**
- Consumes: Task 2 catalog; Stage A `publishCkpt`; `checkFenceOrThrow`.
- Produces the §3 creation sequence (three conditional writes, DDL-rate):
  1. catalog CAS: insert `{ns, Creating, fresh_random_128, creator = my fence identity}`;
  2. `_ckpt` create at `<ns>/<inc>/_ckpt` (Task 4 re-keys; until Task 4 lands this task's
     integration test runs at the Stage-A key with the incarnation carried in the entry only);
  3. catalog CAS: `Creating → Live` — and THIS CAS re-presents the creator's ADMISSION
     GENERATION and token-CASes the OBSERVED entry (TLA Task 3 concern-7 obligation, verbatim).
- `Creating` forbids publication (no ref writes admitted while the entry is `Creating`).
- Stale-`Creating` reconciliation: token-exact CAS AT THE CALL SITE, permitted ONLY after the
  creator's fence is terminal (TLA Task 3 obligation 1: the call-site is where token-exactness
  is enforced, with a stale-token fail-closed test).
- **Same-value obligation, stated correctly** [codex finding 7 — the earlier draft conflated
  two different credentials]: the proven THREE same-value sites are Stage A's trio —
  slot-occupy, `_ckpt` CAS, recovery install — sharing ONE captured `admitted_generation`
  (generation-only; Stage A Task 6 owns it). The CATALOG-ENTRY TOKEN is a SEPARATE credential:
  the `ZombieGoLive` guard at `Creating → Live` combines the creator's admission GENERATION
  with the OBSERVED entry token — two credentials at one site, not a third member of the trio.
  Tests here (the catalog site's OWN three cases, not the Stage-A trio [codex r3 finding 7]):
  generation-stale (fence bump between `_ckpt` create and the `Creating → Live` CAS) → refused
  by generation; token-stale (entry token changed under the creator by concurrent
  reconciliation) → refused by token; both-stale → refused (first check wins, either order
  acceptable).

- [ ] **Step 1: Failing tests**: happy path (3 writes, entry Live, `_ckpt` exists, incarnation
  stable across the sequence); crash after write 1 → reconciliation by a NEW actor refused
  while creator fence is live, succeeds token-exactly after fence terminal; stale token at
  reconciliation → fail closed; publication attempted while `Creating` → refused;
  fenced-out between `_ckpt` create and the `Creating→Live` CAS → the CAS refuses (the
  zombie-install C++ twin — `CaRefCatalogCore` `ZombieGoLive` red); token-stale: entry token
  advanced by a concurrent reconciler between the creator's read and its `Creating → Live`
  CAS → refused by token, entry unchanged; both-stale: fence bumped AND token advanced →
  refused (either check first, both orders exercised).
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate green.
- [ ] **Step 5: Commit** `ca: ref — namespace creation lifecycle; token-exact reconciliation; three-site recheck`.

**TWO MORE CARRIES FROM TASK 3, placed as steps rather than notes.**

- [ ] **Give `isCreatorFenceTerminal` and `reconcileStaleCreator` a production caller.** Task 3 built and
  tested both, but nothing calls `reconcileStaleCreator` outside its own tests: the trigger — something
  noticing a stalled `Creating` entry — needs the catalog-backed discovery this task adds. A tested
  function with no caller rots silently; this campaign already found one test hook that no test set
  (`install_region_probe_for_test`). So wire it here, and pin the wiring with a test that drives
  reconciliation through the discovery path rather than by calling the primitive directly.
- [ ] **Re-check EVERY call site of the birth-`_ckpt` cleanup against the NEW key shape. Derive the count
  when you get here; this step deliberately states none.** It said "three", was corrected to "four" by the
  Task 3 review, and was three again within the hour once fix round 1 removed the unsafe site — the same
  figure went stale twice in one afternoon, in a plan step whose whole purpose was to prevent staleness.
  Run `git grep -n cleanupOrphanedBirthCkptBestEffort` and check what you find. Task 3's
  `cleanupOrphanedBirthCkptBestEffort` deletes a never-born namespace's `_ckpt`, and its safety argument
  is explicitly scoped to today's sentinel `_ckpt` keying. This task re-keys `_ckpt` to
  `<ns>/<inc>/_ckpt`, which moves the object the argument is about. Re-derive the argument at the new
  shape — do not assume it carries — and confirm the never-born-only guarantee still holds, including
  the negative case (a `Live` namespace's `_ckpt` is never deleted by that path). `_ckpt` has no repair
  path, so a wrong delete here is unrecoverable.

**OBLIGATION CARRIED FROM TASK 3 (ruled 2026-07-30, so it has an executor rather than a citation).**
Task 3 builds the creation lifecycle and enforces "`Creating` forbids publication" **at the catalog
level only**. It is deliberately NOT enforced on the production ref-write path, and the reason is a
constraint rather than a preference: consulting the catalog per ref write would add a protocol step to
the write path, which the standing veto forbids and which the insert cost would not survive. So until
this task moves existence and discovery onto the catalog, a namespace sitting in `Creating` does not
actually block production ref writes.

- [ ] **Close the gap here, and make the closure executable.** When the universe comes from the catalog,
  a `Creating` entry must refuse publication on the real path, not only in the lifecycle driver's own
  tests. Pin it with a test that publishes while `Creating` through the PRODUCTION path and is refused —
  if that test cannot be written without a per-write catalog read, say so explicitly and record what the
  refusal actually rests on instead. An invariant asserted only against a driver is an invariant the
  production path does not have.

**Carried from the Task 2 review — the fence seam is real but undocumented.** The brief listed
"Stage A's `publishCkpt`/fence discipline" as consumed, and `publishCkpt` re-checks the fence after the
read and before every CAS. `casUpdateImpl` has NO fence hook in its signature. The seam does exist — a
caller can throw from inside `mutate`, which runs after each fresh read and before each `casPut` — but
nothing obliges a caller to use it and nothing says it is there.

- [ ] **State the fence obligation explicitly** where a catalog caller can see it: that `mutate` is the
  fence re-check point, that it runs after every fresh read, and that a fenced caller MUST throw from
  it rather than checking once before the call. A caller that fences before `casUpdate` and not inside
  `mutate` is fenced against the read it never sees.

### Task 4: Re-key under `<ns>/<inc>/`; universe from catalog; format bump B {#task-4}

**TWO ITEMS PLACED HERE BY THE PLACEMENT SWEEP (2026-07-30), both previously unplaced:**

1. **REGISTER ITEM R9** (`2026-07-28-ref-rework-adjacent-findings.md {#r9-neverborn-fence}`) had
   ZERO mentions anywhere in this plan — the only R-item with none, which is what a late addition
   looks like. The register says "Stage B incarnations close it structurally / Owner: Stage B
   incarnation work", and **a structural closure that nothing asserts is a claim, not a closure**:
   today a two-birth stream leaves a namespace permanently HELD (`UnconsumedSealCrossing`) until an
   operator intervenes. Discharge it HERE, as a test: a straggler birth `PUT` arriving at a DEAD
   incarnation cannot collide with a live re-birth, because the incarnation segment makes the two
   key spaces disjoint. If that proves impractical to drive, the fallback is Task 11's residual list
   with the reason — but the default is the test, and an untested structural claim needs the
   controller's explicit sign-off, not silence. Same family as Task 3's `[CKPT-FAILED-BIRTH-DEBRIS]`:
   both are births that do not land.
2. **The bump-B VERIFICATION**, deferred by Task 1's review as minor 8 to a "final-review triage"
   that does not exist in this plan (`grep -ci "final review"` = 0), so it would simply never have
   been performed. It is one line and it is load-bearing, because Tasks 1/1c re-key AHEAD of the
   bump and Constraint 14 says there is exactly ONE: **confirm this task actually performs format
   bump B, and that Task 4b rides it rather than adding a second.** Assert it, do not eyeball it.

**Files:**
- Modify: `.../CasLayout.h` — NOTE the executed state: Task 1 ALREADY migrated these helpers to
  the `<ns>/<inc>/` grammar, so what changes here is the VALUE, not the shape: the real catalog
  incarnation replaces `stageATransition()` on the discovery paths,
  `.../Pool/CasRefLedger.cpp` (writer + recovery paths carry `NamespaceLifeId`),
  `.../Gc/CasGc.cpp` — `fold` universe (`ref_tables` seeding `:1049` region),
  `discoverUniverse` `:2393` (REPLACED: one catalog GET; the refs-prefix LIST becomes the
  intra-namespace hint only), REBUILD traversal, fsck universe; pool format bump B
- Create: `src/Disks/tests/gtest_cas_universe_from_catalog.cpp`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: spec §5's fold shape — per round ONE catalog `GET`; ref coverage keyed by catalog
  `life_id`; unhinted namespaces carry verbatim; the frontier proof (Stage A
  Task 9) now iterates EVERY `Live`/`Removing` entry. **`UniversePolicy::kDefault` stays
  `StageA_Suppressed` in this task** [codex r2 finding 1 — flipping here would open a
  deletion-capable window before
  the removal lifecycle (Task 5) and read-side/pre-delete contracts (Task 6) exist]; the flip
  is Task 7b's, after Tasks 5/6/7 are green. Recovery/fold/fsck/sweep construct
  `NamespaceLifeId` from the catalog, live readers from their handle (§2).
  `NamespaceLifeId::stageATransition()` remains for the READER paths until Task 6 replumbs them
  — ITS deletion and the tree-wide zero-grep gate move to Task 6 [codex r2 finding 1], and after
  the amendment that gate must also cover the NAMESPACE-FILE call sites (Task 1c widens the
  helpers, Task 4b gives them real values).
- Scope boundary with Task 4b: this task re-keys the REF layer and swaps discovery to the catalog;
  namespace files follow in Task 4b UNDER THIS TASK'S FORMAT BUMP (Constraint 14 — one bump). A
  pool written between Task 4 and Task 4b is a development-only intermediate; there is no
  persisted data and no compat obligation, so the two tasks need not each be
  independently loadable.

**Carried residues — this task opens `CasRefLedger.cpp`, so it owns these two.** Both were filed
in BACKLOG against "whichever task next touches the install regions"; that placement named no task
and the task it meant closed without taking them, which is exactly why they are written as steps
here instead of as a pointer.

- [ ] **Step 0a: the terminal-state-reported-as-retryable arm** (BACKLOG
  `{#lane-terminal-reported-as-retryable}`, re-verified live 2026-07-30). `commitRefChunk`'s "lane
  not `Ready` at new-id allocation" arm sets `RefLaneState::Faulted` and then completes the
  survivors with `makeCasWriteRetryLaterExceptionPtr` — a TERMINAL state reported with the class
  upstream treats as retryable. It is self-limiting (the next flush's `resolveWedgeOnce` takes the
  `invalid_lane_state` arm), so the cost is one spurious retry on a `chassert`-guarded path — but it
  contradicts the contract's own split, where `Faulted` carries `CORRUPTED_DATA`. One-line fix: hand
  that arm the same `CORRUPTED_DATA` class its sibling `Faulted` arms use. Add the assertion that
  pins it; a terminal state must not be reported as retryable from ANY arm.
- [ ] **Step 0b: the stale install-region count.** The comment `Post-durable install region 2 of 3`
  in `CasRefLedger.cpp` still counts three; the restatement deleted the third and exactly TWO
  probe-instrumented regions remain (verified 2026-07-30). `gtest_cas_ref_ckpt.cpp`'s fence comment
  already says "BOTH", so the tree currently contradicts itself about how many places must stay
  allocation-free. Fix the count. (The other half of that BACKLOG entry — the probe having no test —
  is RESOLVED: that fence uses it, and discloses that the probe is SHARED by both regions.)

- [ ] **Step 1: Failing tests**: a NEW test in `gtest_cas_universe_from_catalog.cpp` — fold
  discovers a namespace with ZERO listable objects (hint fully blind) via the catalog and
  probes its frontier (this does NOT touch the Stage-A liar test: after the amendment
  `gtest_cas_list_liar_end_to_end.cpp` has exactly TWO legitimate Stage-B editors — Task 5b, which
  MUST adapt the two capstone sentinels because LIST-independent recovery is precisely the fix
  their failure text instructs, and Task 7b's kill-shot edit [codex r3 NEW-1, amended]. This task
  is still not one of them);
  a `Creating` entry is NOT folded, NOT frontier-required (no publication can exist);
  a `Removing` entry IS frontier-required; keys of a dead incarnation are refused by parsers
  (foreign-prefix inertness — the fold works only off catalog entries); old-format pool open →
  fail closed naming recreation (bump B).
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** Full CA gate + both CA-s3 lanes
  local green. **Step 5: Commit**
  `ca: ref — incarnation-keyed layer; universe from catalog; format bump B`.

### Task 4 SPLIT (2026-07-30) — the task proved to be Tasks 2+3 combined in size {#task-4-split}

**NAMING TRAP, read this before citing anything here.** This section's substeps are **4-A / 4-B / 4-C**, and
they are NOT the tasks **4b / 4c** further down — those are separate tasks (namespace files under `_files/`,
and `_ckpt` strengthening). The names differ only by a hyphen and a capital, which has already produced one
report-file collision: a Task-4-substep report and a Task-4b report both wanted `task-4b-report.md`. When
naming an artefact for a Task 4 substep, spell it out (`task-4-substep-B-…`); when naming one for task 4b or
4c, qualify it by subject (`task-4b-nsfiles-…`).

Task 4's implementer stopped and asked rather than pushing on, which was correct. Its first attempt at
catalog-authoritative discovery produced **127 test failures**, every one the same mechanism: GC, rebuild
and fsck tests write raw ref and manifest objects through low-level helpers without ever touching the
catalog, so a catalog-sourced universe is EMPTY for them. That is the empirical form of the argument that
settled the scope question — discovery without population is not incomplete, it is armed, because the
frontier compares `proven == total` and an empty universe compares `0 == 0`. The working draft is saved at
`.superpowers/sdd/.../task-4-universe-from-catalog-draft.diff`; it was reverted rather than left as 127
reds in a shared worktree.

- **4-A — LANDED.** Format bump B (`kNamespaceLifeKeyedGeneration = 5`, `G_BUILD` 4→5, change points on
  `REF_STREAM`/`REF_CKPT`, the backward floor moved to the new constant) plus Steps 0a and 0b. Note for the
  record what 0a actually was: not the "one-line fix" its BACKLOG entry claimed, because a PRE-EXISTING
  test (`CasAnomalyPolicy.NonReadyAtNewIdAllocationFaultsAndFailsClosed`) asserted the WRONG behaviour as
  correct — a terminal state reported as retryable. **A test pinning a defect is worse than an unpinned
  defect**: it fails when the behaviour is corrected. And the bump needed ~27 golden `"v":4` literals across
  15 files, found by running the gate, not by grep — the first grep missed the escaped-quote form.
  **Method note: for a golden-literal sweep the gate is the search tool and grep is only the hypothesis.**
- **4-B — map the choke points.** Enumerate every helper through which a test writes a namespace's first
  ref object, which files use it, and how many of the 127 funnel through each. This decides the shape of
  everything after it and is therefore its own step: **the obvious fix is 127 `casAdmitEntry` calls; the
  better one is routing the shared helpers through the REAL birth path**, so the catalog populates the way
  production populates it. A test that fabricates a catalog entry by hand proves nothing about the path
  that creates entries — the same reason a driver-only invariant was rejected in Task 3.
- **4-B — DONE. The map, and it collapses to ONE function.** All ten raw-write helpers
  (`writeTxnAt`, `writeSealAt`, `publishAt`, `appendRefLogSeed`, `appendOwnerEvent`,
  `publishCommittedTransition`, `dropRefTransition`, `addPrecommitTransition`, `promoteTransition`) are
  defined in the shared `src/Disks/tests/cas_test_helpers.h` and all drain into `writeRefLogTxnRaw`.
  Failure partition, by terminal reachability, closing exactly: **136 reach only `writeRefLogTxnRaw`,
  21 reach only the real path (`beginPartWrite`/`dropNamespace` — fixed for free by 4-C's wiring), 7
  reach both = 164.** The file-level rollup was WRONG at first (a "20" that split 17+9+2) and was reconciled to
  **28 files = 14 raw-only + 8 real-only + 6 mixed**, each named once, verified by script — the original
  count had silently dropped five single-failing-test files. It was also checked that every one of the 28
  files' failing tests really is in the 164, so nothing is claimed fixed "for free" that was not failing.
  The lesson stands regardless: a classification whose parts exceed its whole is not a partition, and the
  gap is where an unnamed case hides.
  **None of the ten can route through real birth**, and the reason is the same for each: they exist to
  place ref-log bytes at a caller-chosen id and content — holes, out-of-order ids, a table with no
  `_ckpt` — i.e. states `createNamespace`/`completeCreation`'s own invariants (INV-1 contiguity, INV-2's
  seal grammar, the mandatory `_ckpt` publish) refuse to produce. So the fix is `casAdmitEntry` beside
  `writeRefLogTxnRaw`, and **zero test files are edited**.
  Also mapped: ~15 per-file wrapper names that a single-level grep misses; 133 of 164 classified at one
  level, the other 31 needed a two-level trace and all landed in the same ten-helper table.

- **4-C — the wiring and the discovery, with 4-B's map in hand.** Production birth resolves an incarnation
  from the catalog ONCE per table-open (not per write — a per-write catalog GET is a protocol step and is
  vetoed), cached on `RefTableRuntime`; then the saved discovery draft — **which must NOT be re-applied
  verbatim: it predates R11 and contains no fail-closed guard for the empty universe, so re-applying it
  as-is ships catalog-authoritative discovery with the vacuous `0 == 0` frontier still armed** — the R10 incarnation pre-filter ahead
  of `groupRefKeys`, the **R11 empty-universe fail-closed guard**, obligation 3's production-path pin, and
  the `_ckpt`-cleanup re-derivation at the new key shape.

### Task 4b: namespace files under `<ns>/<inc>/_files/` — rebirth waits for nothing {#task-4b}

[Directive design change 2 = the file half of amendment commit 4. Closes the hole the original
ref-layer-only boundary left: an old file omitted by LIST could survive namespace removal and become
visible after the same logical namespace was created again. **This task changes OBJECT IDENTITY
ONLY** — the direct-object implementation of namespace files, manifests and their globally unique
build identities, and loose mountpoint objects are all explicitly unchanged (Constraint 12), and
Constraint 16's operation profile is a hard gate, not an aspiration: `MergeTreeDeduplicationLog`
rotates these files on the insert path.]

**Files:**
- Modify: `.../Pool/CasPlainObjects.cpp` — the four namespace-file operations (`:93`, `:98`, `:103`,
  `:127`) get REAL incarnations instead of Task 1c's placeholder; `.../Pool/CasPool.h`/`.cpp`
  forwarders (`:543-549`)
- Modify: `ContentAddressedMetadataStorage.cpp` (`:1325`, `:1498-1501`, `:1647-1648`, `:1695-1696`,
  `:1846`) and `ContentAddressedTransaction.cpp` (`:822` the read-concat-put append emulation,
  `:1112-1114` the removal loop, `:1235-1237` and `:1410-1411` the copy/move paths, `:1586`) — each
  call site takes the life from the mount's held handle
- Modify: `.../Gc/CasGc.cpp` — TWO distinct sites, do not conflate them:
  (1) `namespacePhysicallyEmpty` (`:2998-3010`) LIST-probes both `manifestNamespacePrefix` and
  `namespaceFilesPrefix` (limit 1) to gate `Pending → Completed`; **its `_files` arm is DELETED** —
  that probe IS the physical-empty proof the directive forbids rebirth from waiting on. The manifest
  arm stays (manifests are unchanged). Whether the predicate survives at all is Task 5's business:
  its terminal-record lifecycle deletes the entry with no physical-empty proof, and Constraint 3
  forbids leaving a branch whose premise died.
  (2) `runNamespaceCleanupPasses` (`:3096`) — its `passes[]` array (`:3135-3138`) pairs
  `{manifestNamespacePrefix, true}` and `{namespaceFilesPrefix, false}`; the `_files` pass stops
  being mandatory removal work and becomes the janitor's leak-only work (Task 5). The per-page/
  per-key `round_still_ours()` + `_cleanup`-marker + `deleteExact` discipline at `:3139-3154` is
  PRESERVED, not loosened — exact-token deletion is what makes leak-only cleanup safe.
- Create: `src/Disks/tests/gtest_cas_ns_file_incarnation.cpp`
- Modify: `src/Disks/tests/gtest_cas_gc_fold.cpp` `:552` (the planted debris key becomes life-scoped)

**Interfaces:**
- Consumes: Task 4 (and rides ITS format bump — Constraint 14, no second bump).
- Produces: `roots/<ns>/<inc>/_files/<relative-name>` as direct objects, and the two consequences the
  directive states: once the catalog entry is removed, files from that incarnation are structurally
  unreachable; and **namespace rebirth must not wait for `_files` to become physically empty** — the
  `_cleanup` LIST-derived physical-empty proof for files is DELETED, not weakened.
- LIST may discover old file debris for lazy cleanup (Task 5's janitor), but **LIST omission may only
  leak storage — never visibility, rebirth or deletion safety** (directive §2, verbatim).
- Request profile UNCHANGED per file operation (Constraint 16): no catalog request, no ref-log
  append, no blob upload, no folder-manifest rewrite, same direct-object backend counts.
- **Why Constraint 16 is not hypothetical — the chain, so the implementer can see the insert path
  it is standing on:** `ContentAddressedMetadataStorage` does not override
  `supportWritingWithAppend` (`IMetadataStorage.h:371`, default `false`), so
  `MergeTreeDeduplicationLog`'s `rotateAndDropIfNeeded` (`MergeTreeDeduplicationLog.cpp:236`) FORCES
  a rotation whenever the disk cannot append (flag set in its ctor, `:95`), and `load` (`:107-114`)
  already special-cases `Plain` and `ContentAddressed`. Dedup-log files reach the namespace-file path
  by directory name (`Parts/PartPathParser.h:75` `kDeduplicationLogsDirName`), and "append" for them
  is the read-modify-rewrite at `ContentAddressedTransaction.cpp:822` (true append on content part
  files is `NOT_IMPLEMENTED`, `:757-758`). Existing coverage of that path shape lives in
  `src/Disks/tests/gtest_ca_wiring.cpp` and
  `src/Storages/MergeTree/tests/gtest_deduplication_log_null_writer.cpp` — read them before writing
  new ones, and keep both green.

- [ ] **Step 1: baseline FIRST, before touching anything** — land
  `NamespaceFileOperationProfileIsUnchanged` against the CURRENT tree, where it must PASS, and paste
  its numbers into the task report. It records the per-operation backend op journal for the four
  shapes the directive names — whole-file rewrite, the append emulation
  (`ContentAddressedTransaction.cpp:822`), remove, and a dedup-log ROTATION sequence (create N,
  rotate, drop the old segment) — as exact counts per family (PUT/GET/HEAD/LIST/DELETE) plus the
  four zeros (catalog GET, ref-log append, blob upload, folder-manifest rewrite). "Unchanged" is
  only meaningful against a recorded number; a test written after the change proves nothing.
- [ ] **Step 2: Failing tests** in `gtest_cas_ns_file_incarnation.cpp`:
  `OldFileHiddenByListIsInvisibleAfterRebirth` — the headline hole: write `_files/f` under inc₁,
  make LIST omit it (the liar backend), drop the namespace, recreate it as inc₂, read `f` → absent,
  and inc₁'s object is still physically there (the point: correctness comes from the KEY, not from
  having deleted it);
  `RebirthDoesNotWaitForFilesToBeEmpty` — creation succeeds with inc₁ `_files` objects present and
  never LISTs `_files` to decide;
  `PhysicalEmptyProofIgnoresFiles` — white-box on `namespacePhysicallyEmpty` (`:2998-3010`): a
  namespace with `_files` debris and no manifests answers empty;
  `LegacyUnqualifiedFileKeyIsRefusedAtOpen` — an old-format pool carrying `roots/<ns>/_files/x` fails
  closed naming recreation (bump B), it is NOT read and NOT migrated;
  and Step 1's profile test, now re-run against the re-keyed tree with the SAME recorded numbers.
- [ ] **Step 3:** → FAIL. **Step 4:** Implement. **Step 5:** Full CA gate + both CA-s3 lanes +
  `test_content_addressed_*` dedup-log-bearing lane green. **Step 6: Commit**
  `ca: ref — namespace files keyed by incarnation; rebirth waits for no file`.

### Task 4c: `_ckpt` strengthened — `O(1)` size, corruption on conflict {#task-4c}

[Directive design change 4 = amendment commit 5. **The sequencing is the directive's own and is
load-bearing: "Delay the conflicting-`life_epoch` behavior change until `_ckpt` has been re-keyed:
before incarnation separation, different namespace lives could still share the old key."** Landing
this before Task 4 would turn a legitimate two-lives-one-key observation into a corruption
exception. Hence: depends on Task 4, and its behaviour half MAY NOT be cherry-picked earlier.]

**Files:**
- Modify: `.../Formats/CasRefCkptFormat.h`/`.cpp` — the field set (`:46` `std::optional<uint64_t>
  life_epoch`) and its strict grammar; the header's merge documentation (`:27`, `:37`)
- Modify: `.../Pool/CasRefCkpt.cpp` — `mergeCkpt` (`:32`) and the per-field semantic-maximum helper
  (`:16`); `publishCkpt` (`:54`) inherits the new join outcome
- Create: `src/Disks/tests/gtest_cas_ref_ckpt_join.cpp`

**Interfaces:**
- The `O(1)` invariant is Constraint 15 and is STATED IN THE HEADER as an invariant, not left to the
  plan: `_ckpt` is a fixed-size product of scalar monotone facts; its encoded size is `O(1)` in
  refs, files, transactions and writer epochs; maps, collections and cardinality-growing fields
  belong in a separate immutable object or ledger.
- The join rules after the re-key (directive §4, verbatim): "unknown `life_epoch` joined with `E`
  yields `E`; `E` joined with `E` yields `E`; two different present `life_epoch` values in one
  incarnation are corruption, not `max`; `checkpoint_snapshot_id` and `last_epoch_seal` continue to
  merge by semantic maximum."

- [ ] **Step 1: Failing tests** in `gtest_cas_ref_ckpt_join.cpp`:
  `JoinUnknownLifeEpochWithPresentYieldsPresent`;
  `JoinEqualLifeEpochsYieldsSame`;
  `JoinConflictingLifeEpochsIsCorruption` — the directive's "conflicting `_ckpt.life_epoch`" test:
  `CORRUPTED_DATA` naming the key and BOTH values, never `max`, and the publisher does NOT CAS;
  `CheckpointAndSealStillMergeBySemanticMaximum` (both directions, including present-beats-absent);
  `EncodedCkptSizeIsIndependentOfCardinality` — encode the `_ckpt` of a namespace holding 1 ref/file
  and one holding 10k; the encoded sizes are EQUAL (Constraint 15's regression fence — the test
  exists to fail the day someone adds a collection);
  plus a compile-time fence in the same TU: `static_assert(std::is_trivially_copyable_v<RefCkpt>)`
  with a comment citing Constraint 15 — verify at execution that today's `RefCkpt` (three optionals
  over integer aggregates) satisfies it; if some field legitimately does not, drop the static_assert
  and say so in the report, leaving the encoded-size test as the fence.
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** Full CA gate green. **Step 5: Commit**
  `ca: ref — _ckpt: O(1) invariant stated, conflicting life_epoch is corruption`.

### Task 4d: opaque physical `life_id`; split stream from state — LANDED, `6a3dd6a9245` {#task-4d}

**Why this is a task and not another bullet in removal.** Tasks 4/4b correctly made every operation
life-aware, but their generation-5 physical grammar repeats the opaque logical namespace in every key
and forces the fold's one global LIST to enumerate namespace files too. Removal is only one consumer of
that grammar: recovery, fsck, REBUILD, mount safety and decommission all parse or derive it. This task
therefore establishes the final physical identity before Task 5 consumes it; Task 5 owns lifecycle and
cleanup policy, not another layout transition.

**The final grammar.** The current catalog field `incarnation` IS the physical `life_id`; this task does
not rename the field, change the catalog wire shape, replace `NamespaceLifeId`, or mass-edit ordinary
reader/writer signatures. The logical pair remains useful in memory. Only object keys drop `.ns`:

```text
cas/ns/stream/<life_id>/_log/<txn>.zst
cas/ns/stream/<life_id>/_snap/<txn>.zst
cas/ns/stream/<life_id>/_cleanup/<txn>       # transitional; Task 5 deletes the class
cas/ns/state/<life_id>/_ckpt.zst
cas/ns/state/<life_id>/_files/<relative-name>
```

`_snap` is deliberately in `stream/`: it is immutable, sequence-addressed by `RefTxnId`, is already
grouped with logs for recovery hints and covered-object cleanup, and is cheap beside `_files`. Moving it
to `state/` would require either a second hot LIST or a simultaneous recovery/cleanup redesign. The
split is therefore immutable stream objects versus point/path-addressed state, not "logs versus all
other objects".

**Files:**
- Modify first: `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md` — INV-3
  physical grammar, mount-safety premise, one hot stream LIST, one paced ownership-tree janitor and
  the cost table. The current "one life, one physical subtree" derivation is superseded, not left
  beside the new one.
- Modify: `.../Formats/CasLayout.h`/`.cpp`, `.../Primitives/CasNamespaceLifeId.h`,
  `.../Formats/CasPoolMetaFormat.cpp`, `.../Formats/README.md`
- Modify: `.../Gc/CasGc.h`/`.cpp` (the round scan and reverse catalog index),
  `.../Pool/CasRefProtocol.h`/`.cpp`, `.../Tools/CasFsck.cpp`, `.../Tools/CasInspect.cpp`,
  `.../Tools/CasDecommission.cpp` (listed-key consumers only)
- Modify: `.../Pool/CasServerRoot.h`/`.cpp` and its pool-open callers (the mount-safety seam)
- Modify mechanically: layout goldens, key-shape tests, integration prefix expectations and soak
  classifiers. Do not rename `incarnation` or `NamespaceLifeId` in this task; that creates broad churn
  without changing an invariant.

**Interfaces:**
```cpp
using NamespaceLifePhysicalId = UInt128;

struct ParsedRefObjectKey
{
    NamespaceLifePhysicalId life_id;
    RefObjectKind kind;
    RefTxnId txn_id;
};

struct ParsedNamespaceFileKey
{
    NamespaceLifePhysicalId life_id;
    String relative_name;
};

String namespaceStreamPrefix(const NamespaceLifeId & life) const;
String namespaceStatePrefix(const NamespaceLifeId & life) const;
```

`parseRefCkptKey` analogously returns the physical id. Keep these three existing family parsers rather
than inventing a tagged union: their payloads and error contracts differ, and retaining their names is
what makes the call-site rewrite mechanical.

The existing point-key API remains source-compatible: `refLogKey`, `refSnapshotKey`, `refCkptKey`,
`namespaceFileKey` and `namespaceFilesPrefix` still take a `NamespaceLifeId`; their implementations use
only `.incarnation`. A life-owned listed key can no longer reconstruct a logical name. An immutable cut
is the decoded bytes + object token from ONE successful catalog GET and the reverse index built exactly
once from those bytes; consumers never patch it with a later GET or mix cuts inside one decision.
Every catalog state participates in `life_id → NamespaceLifeId{name, incarnation}`. A duplicate current
`life_id` is `CORRUPTED_DATA`, not "first row wins": both rows are unresolvable; catalog mutations,
REBUILD, decommission, fold adoption and destructive GC fail closed, while fsck/inspect report and
continue over unrelated unique ids and unrelated point I/O remains available. A `life_id` is never
deliberately reused; the existing random-128 incarnation uniqueness assumption now applies pool-wide.

Absence from a cut is inert-debris proof only when that cut follows observation of the object. The hot
round completes its one stream LIST and then takes ONE authoritative catalog `{token, value}` cut. A
listed id absent from that later cut is dead, inert debris: it is counted for eventual janitor work,
but is neither admitted nor a reason to defer. `Live`/`Removing` ids present in the cut are admitted;
a new admission after the cut could not have appeared in the completed LIST and is next-round work.
Task 5's janitor likewise reads its cut AFTER each LIST page. `_path` must never be invented as
replacement authority, and no consumer adds parent evidence, history, a tombstone or a second catalog
GET to classify the listed ids.

- [x] **Step 0: amend the authority before code.** Land the spec change and its explicit rejected
  alternative: removing the full LIST in favour of an unbounded serial `GET N+1` chase has no bounded
  frontier under a continuously writing namespace and loses the listing's scheduling witness; adding
  an authoritative head would put a CAS on the append path. The adopted hybrid is catalog universe +
  one `LIST(cas/ns/stream/)` scheduling hint + exact arithmetic reads/frontier probe + a separately
  paced leak-only `LIST(cas/ns/)`. No code starts while the authority still requires the old subtree.
- [x] **Step 1: separate parsing from logical resolution while generation 5 still runs.** Change the
  listed-key parsers to return physical id + kind + id, build the reverse catalog index, and resolve
  current objects through it. During this preparatory step the old key still contains a logical name;
  parse it only for a runtime corruption check that a current catalog resolution names the same row,
  never to supply missing catalog state. A mismatch records the physical name and authoritative name,
  suppresses destructive GC, is reported-and-continued by fsck/inspect and never redirects an operation;
  the check disappears with the generation-5 parser in Step 2, not into a `chassert`.
  **Tests:** two catalog rows in any lifecycle states with one current id make both rows unresolvable,
  stop catalog mutation/REBUILD/decommission/destructive GC, and still let fsck plus an unrelated unique
  point read progress; under Task 5's amended final order, a hot-LIST id absent from the sole later
  cut is inert debris and does not defer; changing only the namespace spelling in an old-format key
  produces
  the mismatch disposition and cannot redirect an operation; all point readers/writers retain identical
  request counts.
- [x] **Step 2: introduce the two prefixes behind existing builders.** Add
  `namespaceStreamPrefix`/`namespaceStatePrefix`; route `_log`, `_snap` and transitional `_cleanup` to
  the first, `_ckpt` and `_files` to the second. Delete `refsNamespacePrefix`, `casRefsServerPrefix`
  and every parser branch that tries to recover an opaque, multi-segment namespace from a physical
  life key. This is the deliberately mechanical step: callers of the five point builders do not
  change, while comments/goldens/prefix literals are bulk-rewritten then reviewed by family.
- [x] **Step 3: make the round's one hot enumeration exact in scope.** `enumerateRefPrefix`, defer
  accounting, fold grouping and probe A (until Task 7a deletes it) LIST only `cas/ns/stream/`.
  Plant `_files` and `_ckpt` objects and assert from the backend journal that neither is enumerated;
  plant `_log` plus `_snap` and assert both are offered. LIST remains a scheduling/performance hint;
  catalog + arithmetic exact reads remain the correctness path.
- [x] **Step 4: replace the server-root physical-prefix premise.** A `server_root_id` has owned live
  namespace work iff the mandatory catalog contains a `Creating`, `Live` or `Removing` logical name
  under that root. `serverRootSubtreeEmpty` still probes
  `cas/manifests/<srid>/` and `roots/<srid>/`, because those families retain path identity, but no
  `cas/ns/<srid>/` prefix exists. Thread the catalog observation from the pool layer rather than making
  the low-level server-root module silently decode an optional catalog. Missing/unreadable catalog after
  pool creation is fail-closed; only the existing proven-new-pool bootstrap may initialize it. Match
  ownership by canonical path component, not raw `starts_with`, and pass the same successfully decoded
  observation to BOTH the absent-owner and absent-epoch paths. A conditional-create conflict recomputes
  the catalog + manifest + roots precondition bundle rather than reusing a stale emptiness decision.
  **Tests:** every catalog state blocks owner/epoch recreation; `root/x` does not match `root/xy`; dead
  opaque stream/state debris alone does not; manifest-only and loose-root-only debris still block;
  unreadable catalog does not fall back to physical guesses; and a conflict followed by newly visible
  owned work re-runs the whole precondition for both owner and epoch instead of reusing the old answer.
- [x] **Step 5: rewire recovery tools, not authority.** fsck, REBUILD, inspect and decommission group a
  listed object by physical id and join it to the catalog cut. Before Task 5's codec consolidation,
  REBUILD carries coverage only for ids named by the sole cut taken after its completed hot LIST; a
  listed id absent from that later cut is counted and dropped as inert debris, not converted into a
  phantom logical namespace, and never causes DEFER solely for absence. The janitor gets its separate
  later post-page cut, which alone may nominate an absent id for physical deletion. Neither a
  listed key nor any future `_path` may
  mint a catalog row or authorize a read/delete. Decommission enumerates owned logical names exactly
  from the catalog; it never tries to recover `server_root_id` from a life key.
- [x] **Step 6: format cut and generation pins.** Advance pool generation 5→6 once; generation-5
  `<ns>/<inc>/` pools refuse with a recreate message. There is no dual parser, copy-forward or fallback.
  Regenerate the layout/generation goldens and assert literal `RootNamespace` text is absent from every
  new stream/state key. Task 5 later makes its own generation-7 wire cut; it must refuse this
  generation-6 pool at open rather than accept a second grammar under one generation.
- [x] **Step 7: gate and commit.** Full CA battery, both object-storage lanes and the namespace-file
  request-profile test remain green. Commit
  `ca: layout — opaque life ids split namespace streams from state`.

**Deliberately deferred.** Do not create `roots/<path>` pointer objects in this task: no production
reader consumes them, and a backup that can be mistaken for authority is worse than no backup. Likewise
do not add `_path` merely because the leaf is available. A later `CasInspect` change may add an immutable,
diagnostic-only `_path` together with its reader and explicit stale/missing behaviour; the catalog
already provides complete introspection for active lives.

### Task 5: Removal lifecycle — one proved `Removing → absent` CAS {#task-5}

**Fourth rewrite, 2026-08-01.** The first rewrite derived everything from "the catalog entry is deleted
last". That was unsound while fold cursors were keyed by logical name: `Removing` recreated the cursor
every round and same-name rebirth could inherit it. The second rewrite introduced `RemovalReady` to
make pruning monotone. Task 4d's opaque physical identity plus this task's single catalog-built
`ref_lives` map remove both premises, so retaining the fourth state would preserve a protocol whose
only safety fact is no longer needed. The third rewrite then deleted the catalog row after adopting
the evidence seal in the same invocation. That order is also unsound: a deposed actor can resume its
catalog CAS after a successor seal has added a hold because `gc/state` and `cas/ref_catalog` have
independent tokens. This rewrite reverses that cross-object order without adding durable state.

**The invariant.** `Creating → Live → Removing → absent`. `CasFoldSeal` has one
`ref_lives` map keyed by opaque `life_id`. Its value is one `RefLifeFoldState` containing coverage and
optional cleanup evidence. A pure walk-plan constructor creates exactly one row for every catalog row
whose state is `Live` or `Removing`, and no row for `Creating` or an absent id. Parent
coverage, LIST hints, holds and `_ckpt` observations can enrich an existing row but cannot create one;
REBUILD calls the same constructor. Thus the set of ref-life rows is a function of one catalog cut,
instead of an agreement among five independently guarded producers.

Every GC invocation follows one exact order: acquire the lease and validate the adopted seal; run the
catalog-only pre-fold drain to conclusive completion; complete the hot `LIST(cas/ns/stream/)`; take
ONE fresh authoritative full-catalog `{token, value}` cut; pass that sole cut and the completed LIST
observations to `buildRefWalkPlan`; then decide `DEFER` or perform fold, seal and adoption. The drain
may exact-CAS-delete a complete observed `Removing` row only when the matching parent life row
contains cleanup evidence and no durable hold. Every ambiguous/conflicting CAS is conclusively
resolved; if the same exact row may remain, the invocation aborts before successor publication. There
is no special physical cleanup pass: `_ckpt`, stream and `_files` are inert debris for the perpetual
janitor, while orphan manifests remain the perpetual manifest sweep's work. The adopted parent can
temporarily retain the deleted predecessor row; this is safe because a rebirth gets another `life_id`
and every consumer joins through a fresh catalog cut. There is no pruning wait, physical-empty proof
or second deletion window; the catalog stays O(active + in-flight removals).

The post-LIST cut is load-bearing. Catalog `Creating{name->life_id}` is durable before any object for
that life, and the first stream `PUT` is permitted only after `Live` publication and recovery. An id
returned by the completed hot LIST but absent from the later cut is therefore dead, inert debris: it
is counted and remains eligible for the perpetual janitor, but is neither admitted to the walk plan
nor a reason to `DEFER`. `Live` and `Removing` rows present in the cut are admitted. A new admission
after the cut could not have appeared in the already-completed LIST and is next-round work. A LIST
omission can delay observations only; it cannot manufacture or delete catalog authority. This proof
uses no parent-evidence exception, tombstone/history, second fold catalog GET or fallback classifier.

This is helping, not mutual exclusion. If A stalls after observing a drainable row and B steals the
lease, B independently completes that deletion before B may publish anything. A's later exact catalog
CAS loses or observes absence. Inductively, no successor can add a hold to a row an older adopted seal
made drainable: the mandatory drain removes the catalog row before the successor's fresh cut can admit
it. A deferred invocation still completes the drain, hot LIST, fresh cut and plan construction before
its early return. Healthy `REBUILD` uses the same barrier; damaged-state `REBUILD` has no authoritative
parent and performs zero catalog deletes before adopting a reconstructed baseline.

This deliberately removes the design seam that kept producing omissions. There are no independent
ref cursors and cleanup-item collections, no string `"<namespace>/0"`, no fictitious ref shard zero,
and no five-site lifecycle predicate. Blob-target GC sharding is unrelated and remains unchanged. The
only other cursor in this task is the janitor's opaque backend pagination token: it has one producer,
one consumer and no authority over lifecycle or deletion safety.

**Keep the rewrite mechanical.** Define the new row from the existing coverage fields plus the existing
cleanup-evidence payload, pin its codec, then delete the two old collections and their string-key helpers
in one compile-breaking change. The compiler enumerates access sites; most edits are member/key
substitutions and regenerated format goldens. The serialized removal-admission transition,
`buildRefWalkPlan` and the exact deletion API contain the new policy. Format, fsck and inspection code
are read-only consumers of the row, not additional producers; do not preserve compatibility aliases
after the generation-7 cut.

**Files:**
- Modify: `.../Formats/CasRefCatalogFormat.{h,cpp}` (`removal_started_round` on `Removing`),
  `.../Formats/CasFoldSealFormat.{h,cpp}` (the unified ref-life-row grammar),
  `.../Formats/CasFormat.{h,cpp}` (register `FormatId::GcMaintenanceState = 25`),
  `.../Formats/CasLayout.{h,cpp}` (the separate `<prefix>/gc/maintenance_state` key),
  `.../Formats/README.md`,
  `.../Pool/CasRefCatalog.{h,cpp}`, `.../Pool/CasRefLedger.{h,cpp}`, `.../Pool/CasPool.{h,cpp}`,
  `.../Gc/CasGc.cpp`, `.../Gc/CasGcShardPlan.h`, `.../Tools/CasFsck.cpp` (REBUILD's universe)
- Create: `.../Gc/CatalogLifecycleReconciler.{h,cpp}`;
  `.../Formats/CasGcMaintenanceStateFormat.{h,cpp}` (the one cleanup-only `cas/ns/` pagination token,
  stored independently of safety/adoption state); `src/Disks/tests/gtest_cas_gc_maintenance_state_format.cpp`;
  `src/Disks/tests/gtest_cas_ns_removal_lifecycle.cpp`
- Models: `docs/superpowers/models/CaRefCatalogCore.tla`, `CaRefPreFoldDrainCore.tla`,
  `CaRefDeltaIntakeCore.tla`, `CaRefNsCleanupStaleLeaderCore.tla`, `CaRefLaneCore.tla`

**Interfaces:**
- Consumes: Tasks 2–4d, and step 1 below, which is already landed as `a600c2e433c`.
- Produces: the catalog-built ref walk plan; one proved exact `Removing → absent` mutation; the typed
  retryable refusal; the paced perpetual janitor; the reader-absence predicate answered from the
  catalog cut.

**Execution status, 2026-08-01.** The opaque-id layout, mandatory fail-closed catalog and atomic
bootstrap prerequisites are landed as `6a3dd6a9245`, `2b8475fc6f6` and `21ce9e99f4d`. Step 1's core
non-minting change is landed as `a600c2e433c`; its two explicit follow-ups below remain open. The TLA+
phase-0 gate is being completed by this design amendment before C++ work resumes. Production Steps
2–11 are not implemented: the tree still contains the legacy fold collections, cleanup marker and
removal lifecycle. Checked boxes in the Task 5 body therefore mean completed prerequisites or model
gates, never implied production implementation.

#### Step 1 — reads and removals stop minting (LANDED, `a600c2e433c`) {#t5-step1}

Kept here for the record: the three ref-layer entry points no longer mint. Two follow-ups it left open
belong to this task.

- [ ] **Split the zero-mutation pin per entry point.** One test each for `listRefs`, `resolveRef` via
  DROP DETACHED, and table-dir removal, and each must pin **zero catalog mutation in the operation
  journal** — final byte-equality permits mint-then-delete.
- [ ] **`DROP` may cancel a stalled `Creating`; an ordinary read may not.** Step 1 made a `Creating`
  entry read as absent everywhere, which turned `DROP` into a no-op and left a dead creator's entry
  blocking recreation. Restore the asymmetry as a **lifecycle** capability: live creator fence → typed
  retry-later, zero mutations; terminal fence → exact-CAS-delete the complete observed `Creating` row,
  and perform no physical delete. A surviving `_ckpt` is under an id no catalog row names; the new life
  gets a different id and the janitor reclaims it. This removes the reconciler race rather than testing
  an ordering around it. **Three tests:** live fence → zero mutations; terminal fence → exactly one
  catalog mutation, rebirth with a different id and eventual janitor cleanup; concurrent reconciliation
  → either the exact row CAS wins or it conflicts, and `DROP` issues no `_ckpt` delete in either case.

#### Step 2 — one narrow deletion transition {#t5-step2}

- [ ] **Close positive append admission before publishing `Removing`.** Under the local append lane,
  serialize `Live → Removing` with the catalog CAS: once `Removing` is observed, no already-held
  runtime/handle may reopen a positive append lane. Keep admission closed while a catalog CAS retries.
  If the operation fails before a durable transition, it may reopen only after a fresh exact catalog
  observation still proves `Live` under the same life and fence; otherwise it fails closed. The terminal
  append follows under that same admitted removal ownership. This is the lifecycle admission bound, not
  a fresh-name-resolution check.
- [ ] **Red-first admission tests.** Deterministically pause a cached writer holding its life across
  the catalog transition and prove it cannot append positive ownership after `Removing` is visible.
  Separately force a catalog CAS retry and a fence change: admission remains closed during the retry,
  and a failed pre-durable attempt reopens only after the fresh exact `Live` observation under the same
  life/fence. Preserve the separate unauthorized-terminal test in Step 5.
- [ ] Add immutable `removal_started_round` to `Removing`: sample the adopted `gc/state.round` before
  `Live → Removing`, store both changes in that catalog CAS, and reject the field on `Creating`/`Live`.
  It is diagnostic age, not a fence; a stale sample can only surface an already-`Removing` row
  conservatively early. Codec shape and boundary-size tests cover the field. Do **not** add a fourth
  catalog state.
- [ ] Export one narrow GC mutation `deleteCompletedRemoving(exact_observed_row,
  authoritative_parent_row)`. It succeeds only when the exact catalog row is still `Removing`, the
  matching row from the currently adopted parent seal has cleanup evidence for that life, and the same
  row has no durable hold. Absence of evidence never proves completion. Invoke it only from the
  mandatory pre-fold drain after lease acquisition; no post-adoption tail or detached finalizer may
  present a historical seal. Thread the GC leader generation as an authorization fence, but do not
  mistake a separated fence re-read for cross-object atomicity: successor publication is prevented by
  the helping barrier below.
- [ ] **No-hold precondition, enforced rather than derived.** Plant a hold in the same life row, attempt
  deletion and require refusal. A second sabotage supplies evidence for another id; it also refuses.
  The API consumes one coherent catalog row plus one coherent fold row, not a caller-computed boolean.
- [ ] **Catalog deletion is exported for exactly two shapes.** The other is Step 1's separate
  `cancelStalledCreating`, requiring the exact observed `Creating` row and a terminal creator fence.
  There is no generic remove-by-name mutation. **Negative tests:** `Live` is never deletable; an
  unproved, mismatched or held `Removing` row is not deletable; a live-fenced `Creating` is not
  deletable; and the admission-only path cannot carry a removal. This relocates the impossible
  fabricated-missing-entry anomaly test: entry loss is prevented at the mutation API, not inferred
  from byte-identical debris.

#### Step 3 — one catalog-built ref walk plan {#t5-step3}

The key-set equality is the proof boundary:
`keys(plan.ref_lives) = {row.life_id | row.state ∈ {Live, Removing}}`. Tests attack that boundary by
making each adapter attempt to mint a row; they do not duplicate a lifecycle predicate at each site.

- [ ] **Carry the complete round catalog snapshot, not `live_incarnation`.** The lossy map cannot
  distinguish absent from `Creating`, yet this task's hint classifier, plan builder,
  deletion API and janitor require exactly those distinctions. `Gc::fold` and REBUILD take exactly ONE
  plan/intake `CasRefCatalog::Snapshot` after the completed hot LIST and thread it through the
  constructor and destructive classifier; no per-namespace catalog re-resolution is permitted. The
  pre-fold drain's catalog observations and complete rescans are separate operations and are never
  reusable as the plan cut. Task 6 generalizes the same seam to the remaining
  reader/fsck/decommission call sites and deletes the sentinel fallback.
- [ ] Replace `per_ns_shard` and `ns_cleanup_items` with
  `map<life_id, RefLifeFoldState{RefCoverage, optional<RefCleanupEvidence>}> ref_lives`. Delete
  `cursorKey`, `parseCursorKey`, the `"<namespace>/0"` grammar and every ref-shard-zero branch. The
  strict codec accepts each canonical fixed-width `life_id` once, rejects duplicates and rejects the
  generation-6 split grammar at the generation-7 pool-open cut. `RefCleanupEvidence` carries only terminal
  and removal transaction evidence; its owning `life_id` is the map key. It carries neither a duplicate
  logical name/incarnation pair nor a redundant `Pending`/`Completed` state.
- [ ] Implement one pure `buildRefWalkPlan(catalog_cut, inputs)` entry point. `inputs` contains parent
  coverage, listed hints, holds and checkpoint/tail observations. The function first creates the row
  set from `Live`/`Removing` catalog rows, then its internal adapters may only attach data to those rows:
  matching parent coverage, a listed hint resolved through the cut's reverse index, a matching hold,
  and `_ckpt`/arithmetic-tail evidence. A parent row whose id is absent or non-walkable is counted,
  logged and dropped. The hot LIST completes before the sole authoritative cut; a listed id absent
  from that later cut is counted as inert dead-life debris, is not admitted and does not cause
  `DEFER`. None sets pool-wide suppression merely because an old ref-life row exists. Freeze the
  returned plan's key set and all admitted input evidence; the sole target `emplace` remains inside the
  catalog loop. Folding consumes the plan as const input and writes newly earned cleanup evidence only
  into its separate `FoldResult`/successor-row output, never back into the plan.
- [ ] **C++ cleanup pin for cp2/cp6.** Rewrite the stale comment in `Pool/CasRefCatalog.h` which says
  production does not enforce catalog `Creating` before life objects and `Live` before the first stream
  PUT. The production ordering is already authoritative; this formal amendment records the pending
  comment cleanup but does not stage C++.
- [ ] **REBUILD calls the same constructor.** It may provide different coverage observations, but it
  cannot own a second row-admission rule. Tests compare the key set produced by ordinary GC and REBUILD
  from the same catalog cut and sabotage each adapter in turn; `Creating` and absent ids
  remain unrepresentable even when parent coverage, a LIST hint, a hold or `_ckpt` names them.
- [ ] **The adopted parent authorizes deletion and may remain adopted.** After the pre-fold catalog CAS,
  a deferred invocation leaves that parent in `gc/state`; a folding invocation's first new plan drops
  the absent id before any adapter runs. **Tests:** drain and then DEFER, create the same logical name
  with a new `life_id`, and prove the planted predecessor row is neither work nor suppression; then run
  one folding invocation and prove that only the new id enters the successor output.

#### Step 4 — delete special removal cleanup; keep the two evidence failures distinct {#t5-step4}

- [ ] **Delete the marker-driven `Pending → Completed` handshake.** The fold that consumes the
  terminal writes cleanup evidence into the existing life row directly; its presence is durable evidence of
  the terminal fold, not a claim that physical deletion succeeded. Delete the whole special
  post-adoption namespace-removal cleanup pass together with its pending/completed phases and
  `attempted`/`deleted`/`leaked`/`suppressed` replacement proposal. It has no safety role, cannot reuse
  the fold's ephemeral `suppress_destructive` verdict in the next invocation, and a stalled
  name-scoped manifest cleanup can outlive catalog deletion plus same-name rebirth. No replacement
  marker, durable cleanup queue, suppression bit or lifecycle-specific physical pass is introduced.
  Delete `RefNsCleanupState`, the separate item's wire record and its `state` field; re-pin the unified
  life-row grammar and recalculate its per-entry reservation constant. The generation-7 recreate-only
  cut rejects the old `pending`/`completed` field shape when the pool opens.
- [ ] **A terminal record unreadable BEFORE it folds is lost evidence.** Removal legitimately blocks in
  `Removing`; surface it as a terminal-corrupt stuck removal. **Do not promise `REBUILD` as the escape**
  — nothing establishes it can reconstruct that exact terminal. The credible exits are restoring the
  object or recreating the pool, and the message must say so rather than naming a verb that may not
  work.
- [ ] **A terminal unreadable AFTER it folded is a cleanup failure and stays leak-only.** The evidence
  is already durable in the ref-life row; the terminal object matters only to later physical
  reclamation. Entry deletion lands and the residue remains ordinary janitor/orphan-sweep work under a
  **non-suppressing leak counter** — not an anomaly, which would let physical cleanup back into the
  lifecycle through a side door.
- [ ] **The no-gating proof becomes literal again.** `NamespaceRemovalDoesNotListOrDeleteFiles` pins
  that lifecycle completion performs no namespace-owned physical LIST and no physical delete. Plant
  `_files`, `_ckpt`, stream residue and a namespace-scoped manifest; require cleanup evidence to become
  durable, the next invocation's catalog-only drain to delete the row, and every planted byte to remain
  at that exact moment. Then drive the perpetual janitor and orphan-manifest sweep and require eventual
  reclamation. This is the executable proof that physical emptiness cannot creep back through failure
  handling.

#### Step 5 — next-invocation pre-fold drain, then successor publication {#t5-step5}

- [ ] After lease acquisition, read and validate the authoritative adopted parent seal, then read one
  complete catalog snapshot. For every exact `Removing` row whose matching parent life row has cleanup
  evidence and no hold, perform the exact catalog CAS. The catalog token covers the whole object, so
  this is a serial rescan: select one eligible row deterministically from a complete snapshot; after
  every committed, rejected or ambiguous CAS outcome, reread the complete catalog and restart the
  selection. An absent or different exact row resolves the observed candidate; the same exact row
  requires a bounded retry or aborts the invocation. A restarted scan includes every newly observed
  row whose parent life row supplies the same ready/no-hold proof; a deadline or unreadable catalog
  aborts rather than publishing around perpetual churn. **No defer decision, hot stream LIST, catalog
  cut, plan construction, fold, seal PUT or successor `gc/state` CAS may occur while an eligible row
  is unresolved.** Only a complete rescan with no eligible row permits the completed hot stream LIST;
  after that LIST, take ONE fresh authoritative full-catalog `{token, value}` cut, construct the sole
  `buildRefWalkPlan` intake from that cut plus the completed LIST observations, and only then decide or
  publish. The drain performs zero physical LISTs/deletes and does not consult `suppress_destructive`.
- [ ] **The stale-leader/helping test is the cross-object proof.** A observes an eligible row and stalls
  before its catalog CAS. B steals the lease, independently drains that row, completes the hot LIST,
  takes the fresh cut and
  adopts the successor; in that trace A's old exact CAS loses. An already-issued A CAS may instead
  land before B's drain only while the adopted proof remains current; B then observes absence and
  continues the same serial drain. Sabotage B to skip the drain, add a hold in its successor seal, let
  A delete, and require the invariant to fail. The operation journal pins `B lease CAS -> exact catalog
  CAS resolved -> completed hot LIST -> ONE fresh catalog cut -> buildRefWalkPlan -> successor seal
  PUT/adoption`.
- [ ] **DEFER and `REBUILD` cannot bypass the barrier.** A deferred invocation deletes eligible rows,
  completes the hot LIST, consumes the later cut and builds the plan before its early return. Healthy
  `FORCE REBUILD` acquires the lease, drains from the authoritative parent, completes the hot LIST and
  only then takes its rebuild cut. With absent/undecodable
  `gc/state`, `REBUILD` performs zero
  catalog mutations based on a seal discovered by LIST; it may adopt a reconstructed baseline, whose
  eligible rows are drained by the next authoritative invocation. A stale pre-lease rebuild cut is
  never used for successor construction.
- [ ] **Crash and CAS-resolution pins.** Stop after evidence adoption, before catalog CAS, after the CAS
  response is lost, and after committed deletion. In every case the next invocation either resolves
  the exact row or aborts before successor work; immediate same-name rebirth gets a different
  `life_id`, and the deliberately retained `_ckpt` is eventually deleted only by the janitor. No
  physical delete is a precondition for the catalog CAS.
- [ ] **Unauthorized terminal append is refused.** A non-owner without a claimed fence cannot append the
  terminal record. A happy-path owner test cannot catch this check's removal.

#### Checkpoint 5.5 — TDD/TLA gate for the post-LIST cut {#t5-checkpoint-5-5}

- [x] **Gate before any Step-6 C++ resumes.** Amend the existing single owner
  `CaRefPreFoldDrainCore`; do not create a second protocol owner. Model the exact sequence
  `adopted seal -> conclusive catalog-only drain -> completed hot LIST -> ONE fresh catalog
  {token,value} cut -> buildRefWalkPlan intake -> decision/adoption`, together with catalog
  `Creating` before life objects and stream `PUT` only after `Live`/recovery.
- [x] **RED controls and GREEN witness are mandatory.** Carry opaque life id separately from
  `Creating`/`Live`/`Removing` through every observation and cut, and make `plan_lives` a set of life
  ids. A cut-before-LIST sabotage executes through concurrent rebirth, `Live`, stream PUT, completed
  LIST and stale-cut plan construction, then violates semantic `ListedCurrentLifeIsAdmitted`; the
  explicit order invariant remains additional. An absent-listed-means-unknown sabotage violates the
  inert-debris/no-DEFER invariant. An identity-blind predecessor delete violates
  `PredecessorProofCannotDeleteSuccessorRemoving`. The honest model stays GREEN and reaches a witness
  where an old issued request conflicts, the predecessor stream survives, the successor legally reaches
  `Removing`, and the later cut admits/adopts only that successor without debris `DEFER`. Update runner,
  aggregate accounting and results if the configuration count changes. Preserve redirected logs and
  child analysis as the TDD evidence.
- [x] **Focused command:** run `docs/superpowers/models/run_prefold_drain.sh >
  build/task5_review1_prefold_full_gate.log 2>&1` from the repository root. The committed runner
  asserts all eighteen named RED/GREEN/witness outcomes; the report records the direct RED and GREEN
  TLC commands and independent child log analysis.

#### Step 6 — the refusal has no assistance protocol {#t5-step6}

- [ ] Typed retry-later for `Removing`, naming the terminal-fold/removal completion it waits for. Never
  `LOGICAL_ERROR`. **Test:** the eventual success is driven by the real removal sequence, not by a
  fixture erasing the entry.
- [ ] `CREATE` may wake the existing scheduler, then returns retry-later. It may not fold a terminal,
  run cleanup, drive a GC round or mutate the predecessor. **No wait loops, no new worker and no second
  deletion driver. Test:** while the predecessor is `Removing`, `CREATE` performs zero durable
  mutations beyond the in-memory wake; after the real GC deletion it succeeds on its ordinary path.

#### Step 7 — rebirth {#t5-step7}

- [ ] **Test that actually tests same-name rebirth.** Pre-install nonzero predecessor coverage, reuse the
  same writer epoch, run the real removal, and **prove the same `RootNamespace` was reused** — an
  `Atomic` fixture silently mints a fresh UUID and therefore never exercises this. Rebirth immediately
  while the old ref-life row is still the adopted seal, then run the first post-deletion GC round.
  Assert the new `life_id` receives a new row with zero coverage,
  while the predecessor id is absent. Keying by `life_id`, not logical name, makes inheritance
  structurally impossible.
- [ ] **Rebirth runtime acceptance contract.** A runtime captured under the predecessor remains bound
  to that life and never observes or mutates successor state; a fresh name lookup after rebirth gets a
  distinct runtime. Stop an old reader before its state lock, an old append before enqueue, and an old
  publisher before completion; delete and rebirth the same `RootNamespace`; prove each old operation
  stays on the predecessor and returns its stale-or-`NotFound`/retry outcome. A late predecessor
  invalidation must not detach the successor. A self-remount creates a new runtime for the same durable
  life, and `confirmExactRef` on a missing name creates neither slot nor runtime. Checkpoint 7.5 is the
  sole implementation owner of this contract; Step 7 adds no second cache/runtime mechanism.

#### Checkpoint 7.5 — semantic boundaries before the janitor {#t5-checkpoint-7-5}

**This checkpoint is mandatory after the focused `FencedOut` + N+1 drain fix is independently reviewed
and before any Step-8 code.** It creates semantic ownership boundaries now; Task 13 remains the later
behavior-preserving mechanical split of the large translation units after the performance baseline.

- [ ] **Extract one synchronous `CatalogLifecycleReconciler`.** It alone owns selection of an eligible
  adopted-parent `Removing` row, the exact catalog CAS, the mandatory resolution read and the N+1
  rescan loop. Its result keeps two facts orthogonal: `AuthorityStatus::{Authoritative,FencedOut}` and
  `CatalogResolution::{DrainComplete,ExactRowAbsent,ExactRowReplaced,ExactRowStillPresent}`; it also
  carries the exact `retired_lives` proved by completed per-row resolutions and the final catalog cut
  only for `DrainComplete`. An unreadable resolution propagates and produces no result. `DrainComplete`
  covers both N=0 and the final clean rescan after N deletions.
  `FencedOut` remains the control outcome even when the resolution cut proves the predecessor absent or
  replaced. Either conclusive per-row resolution appends exact-life retirement independently, but it
  can never restore authority. Inside the component, an authoritative absent/replaced resolution
  permits the next deterministic scan and `ExactRowStillPresent` retries within the bound or aborts.
  At the caller boundary only `{Authoritative,DrainComplete}` permits LIST,
  plan construction, fold or successor publication. The component performs no hot LIST, ref walk,
  fold, seal publication, `gc/state` mutation or janitor work. **Tests:** retain the direct absent and
  replacement winner races, the stale-leader no-successor-work journal pins and the exact N-row N+1
  catalog-read assertion as the component's contract.
- [ ] **Freeze `RoundInput` and `RefPlan` after construction.** After the reconciler returns an
  authoritative conclusive drain, one completed hot stream LIST and its later full-catalog cut build a
  single `RoundInput`. The sole `buildRefWalkPlan` consumes it and returns one `RefPlan`; downstream
  DEFER, fold, frontier and publication paths receive const views and may not rediscover, append,
  reinterpret or enrich walk targets. Newly earned cleanup evidence is written to the separate
  `FoldResult`/successor rows. Normal and healthy `REBUILD` use the same builder. **Tests:** every
  producer named by the proof changes only the builder's input, and a stale/pre-drain cut or post-build
  target injection cannot affect the consumed plan.
- [ ] **Install the name-slot plus immutable-life runtime cache.** Keep logical routing separate from
  physical mutable state: `RootNamespace -> RefNameSlot -> RefLifeRuntime`, with the runtime indexed by
  `(life_id, admitted_fence_generation)`. `admitted_fence_generation` is the existing local mount-fence
  generation captured when the runtime is created. The raw generation advances on both
  `tripMountLost` and `armMountFence`; no runtime may be published while fenced, and the replacement is
  published only after `check_fence_or_throw` accepts the value produced by the successful re-arm. A
  failed remount or a fence-loss-only generation cannot mint/select a usable runtime generation. The
  name slot is a non-authoritative single-flight cache; only a
  catalog observation may bind it. It does not keep an evicted runtime alive. Remove in-place
  `prepareInvalidatedRuntimeForReuse`, preserve exact-life capture for long operations, and make old
  handles structurally unable to target a successor. Extend `CaRefLaneCore` with simultaneous old/new
  runtime identities and sabotages for old-handle-to-current-slot retargeting; prove immutable runtime
  identity and that predecessor work cannot mutate successor objects.
- [ ] **Create a separate leak-only `GcMaintenanceState`.** Store the janitor's opaque pagination token
  under `<prefix>/gc/maintenance_state` with `FormatId::GcMaintenanceState = 25` and its own token-CAS
  discipline, never inside `gc/state`, the adopted seal or any
  safety/adoption CAS. Losing, repeating or resetting maintenance progress may leak or repeat work only;
  it cannot change round authority, frontier completeness, DEFER, catalog lifecycle or successor
  publication. Corrupt/oversized/backend-rejected progress makes the current janitor page delete
  nothing. An absent object means an empty cursor; first publication uses conditional create. A create
  conflict or token-CAS conflict adopts no local progress and leaves a later scheduled attempt to
  reread/repeat. Corrupt progress is replaced with canonical empty progress only by exact-token CAS;
  losing that reset changes nothing else. Pin format-registry, codec, layout, absent/create-conflict and
  corrupt-reset tests before Step 8 consumes the type.
- [ ] **Checkpoint gate:** focused reconciler tests, immutable-runtime races, cache/remount/shutdown
  tests, `CaRefLaneCore` safe/sabotage configurations, the full prefold TLA runner, and the complete CA
  unit gate are GREEN and independently reviewed before Step 8 begins.

#### Step 8 — the perpetual janitor {#t5-step8}

- [ ] **One bounded scan over the ownership tree, separate from the hot stream LIST.** Task 4d makes the
  round's correctness/performance enumeration `LIST(cas/ns/stream/)`; it deliberately does not pay for
  `_files` or `_ckpt`. The janitor alone walks `LIST(cas/ns/)`, at a fixed page/key budget per round,
  storing ONE cleanup-only backend cursor in the separate `GcMaintenanceState` object. The cursor has no
  field, alias or publication path in `gc/state` or an adopted seal. It is opaque, size-bounded and reset at
  end-of-tree so a later cycle can see an object omitted from an earlier one. A malformed, oversized or
  backend-rejected cursor fails closed for that page/round: surface the error, perform zero janitor
  deletes from a substituted cursor, reset only the durable cleanup progress safely, and let a later
  normally scheduled round begin at the start.
  Keep the work in `namespace_cleanup`; publish `janitor_pages`, `janitor_keys` and
  `janitor_deleted` for Task 11's inventory.
- [ ] **One physical classifier, joined to the catalog authority.** Parse the fixed family and
  `life_id`; do not reconstruct a `RootNamespace` from the key and do not consult `_path`. The immutable
  post-page cut's reverse index classifies an id currently named by any `Creating`, `Live` or `Removing`
  row as retained. An id absent from this cut is a dead-life candidate because the LIST
  page preceded the cut. The fold uses the same observation-before-cut proof at whole-hot-LIST scope,
  but only this janitor path may turn absence into a physical deletion candidate. A malformed key under
  either namespace family is anomaly-and-continue; a loose object under `roots/` is outside this scan
  entirely.
- [ ] **Creation-before-object ordering closes the new-life race.** Catalog `Creating{name→life_id}` is
  durable before any stream/state object of that id. The janitor orders each page as
  `LIST page → fresh catalog GET/decode → classify page → exact-token deletes under the GC fence`.
  One catalog read PER PAGE is sufficient and is the required shape: an object already returned by the
  LIST but absent from the later cut cannot belong to a concurrent creation, because that creation's row
  would have preceded the object; a creation after the cut cannot have placed an object into the earlier
  page. Do not reintroduce one catalog GET per key. A duplicate current id is the Task-4d catalog
  corruption path and suppresses the page's deletes; exact-token mismatch retains a stale-writer rewrite.
- [ ] The janitor is **perpetual** and there is no lifecycle-specific bounded attempt: it is the only
  reclaimer of a dead life's objects after the catalog row that named the id has gone. Folding it into removal would
  make a LIST omission a permanent leak; retaining the row until physical emptiness would put storage
  liveness back on the lifecycle path. Suppression performs no delete, cursor progress may still be
  retained only under the existing safe publication order, and a later cycle retries the key.
- [ ] **Tests:** a suppressed round deletes nothing; a token mismatch retains; an unparseable stream or
  state key records and continues; restart mid-scan resumes from the durable cursor; end-of-tree resets
  it; invalid, oversized and backend-rejected cursors surface, delete zero janitor objects and reset only
  durable cleanup progress for a later round; cursor-update CAS failure does not fail
  removal; and a removed namespace whose ONLY residue is `_files`, omitted for one complete cycle and
  returned in the next, is eventually reclaimed. Assert **bytes were actually deleted**, not merely
  that cleanup evidence exists. Add the symmetric `_ckpt`-only case produced by a stop after stalled
  `Creating` cancellation, plus a catalog-first concurrent creation whose new object is never deleted.
  The concurrent-creation test records exactly one janitor catalog GET for a multi-key page, so the
  page-cut proof cannot regress into O(keys) point revalidation.

#### Step 9 — the `_cleanup` class and the `Removed` snapshot die together {#t5-step9}

- [ ] Rewire **fresh name resolution** onto the catalog cut first — `Removing` or absent
  resolves as absent — then delete the `_cleanup` marker, its publication, the marker-driven
  `Pending → Completed` promotion and the `Removed` lifecycle snapshot. Delete `namespaceIsRemoved`:
  hot paths already holding a life handle retain the stated stale-or-`NotFound` contract and must not
  add a catalog request, while a new caller cannot obtain that handle through the catalog cut. Leaving
  either artefact keeps a physical-empty vestige alive with no reader.
- [ ] **Test:** plant an old-life file and prevent its cleanup, then assert the backend still holds the
  bytes while a **fresh name resolution** answers absent — otherwise absence may hold merely because the
  object vanished, which would pass while the predicate was wrong. A separately retained stale handle
  may still return the old bytes or `NotFound`, never bytes from a reborn life. Assert from the operation
  journal that no `_cleanup` object is ever written.

#### Step 10 — diagnostics that do not suppress {#t5-step10}

- [ ] An input ref-life row whose id has no walkable catalog row is **counted and logged** — ProfileEvent
  plus one line naming the id — then dropped by `buildRefWalkPlan`, and **does not set
  `suppress_destructive`**. It cannot become output work because adapters cannot mint rows. The shape
  means stale input state or corruption, but retaining it would recreate the unbounded dead-cursor
  history and suppressing on it would bring back the measured pool-wide stall. Alert-and-discard.
- [ ] Stuck-removal surfacing fires when a `Removing` row has no matching cleanup evidence for N rounds;
  compare the current adopted round with the row's immutable `removal_started_round` using subtraction
  guarded by `current >= started`, not overflow-prone `started + N`; validate N is nonzero. If folding
  recorded an exact-read failure, name that unreadable key; otherwise report only that no terminal has
  folded — the catalog does not store a terminal id, so no diagnostic may fabricate an exact probe.
  There is no `Pending` corruption case: the optional evidence is absent until the terminal folds.
  **Test:** no signal through round N−1, first signal at N, per-round behaviour
  after, persistence across restart, the `UINT64_MAX` boundary, both absent and unreadable diagnostics,
  and GC appends nothing.

#### Step 11 — capacity, models, hygiene {#t5-step11}

- [ ] **Capacity is intentionally boring.** Charge one worst-form `RefLifeFoldState` per catalog entry,
  including coverage, hold and optional cleanup evidence. Separately over-cover `btr` rows per run
  segment and `cnd` rows per GC shard; neither is charged per entry. Refuse admission loudly; removal is
  never refused. There is no separate cursor/`nsc` index-set proof because there is only one ref-life row.
- [ ] **TLA phase 0, gated before code:** `CaRefCatalogCore` models catalog-only deletion with positive
  cleanup evidence, the no-hold precondition and exact observation, with no cleanup-attempt variable or
  sabotage. A new focused `CaRefPreFoldDrainCore` owns the two-GC-actor protocol: adopted-parent proof,
  exact and ambiguous catalog CAS outcomes, helping after takeover, and the mandatory barrier before
  DEFER, ordinary successor or `REBUILD` adoption. Its primary sabotages let those three decisions
  bypass unresolved drain debt; a separate sabotage reproduces stale A deleting after B has adopted a
  held successor. Its two-row companion returns every external resolution to a complete rescan and
  makes a stale/non-exact delete go red. The pre-fold model also owns the single cut-to-consumer
  interface: it carries opaque life id separately from lifecycle state, plus the immutable full-catalog
  token from `TakeFreshCut`, into life-id-keyed ref-plan intake;
  `_sab_intake_uses_predrain_cut` substitutes the earlier drain observation and violates
  `IntakeConsumesFreshPostDrainCut`; `_sab_intake_uses_stale_token` holds the fresh row value constant
  while substituting only the earlier full-catalog token at the plan/adoption seam, so both halves of
  the provenance pair are independently load-bearing. The same owner models a completed hot LIST
  before `TakeFreshCut`: a consequential cut-before-LIST sabotage and an absent-listed-means-unknown
  sabotage pin semantic current-life admission, order and inert-debris classification. A separate
  identity-blind delete sabotage proves predecessor evidence cannot delete successor `Removing`, while
  the rebirth witness retains predecessor bytes, exercises a real old-request conflict and reaches
  successor `Removing` adoption without perpetual `DEFER`. `WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE`
  proves a drained `Removing` row is absent from the consumed cut and plan. `CaRefDeltaIntakeCore`
  remains only the walk-plan key-set/fold proof; do not duplicate adopted-parent or drain ordering
  there, and do not cite its unrelated `_sab_adoptbeforecommit`/`NoMissedFold` pair as provenance
  evidence.
  `CaRefNsCleanupStaleLeaderCore` is retargeted to the perpetual janitor's captured physical-id
  nomination after the special removal pass dies.
  **Capture of physical identity by perpetual cleanup stays, with its capture-time test and at least one
  stale-leader-after-rebirth data-loss sabotage** — ordering does not revoke a running actor's local
  copy, so janitor/orphan-sweep work must never re-derive a reborn target from a logical name.
- [ ] **Task 7 gets no removal-finalization branches.** A cataloged `Removing` row remains owned and is
  recovered under the claimed writer fence; `Removing` without `_ckpt` is corruption because no
  lifecycle path deletes the checkpoint before the catalog row. Once GC deletes the row, decommission
  has no logical owner to finalize and the perpetual janitor owns all opaque residue.
- [ ] Hygiene, one line each: delete `per_ns_shard`, `ns_cleanup_items`, their string-key helpers and
  every false comment about ref shard zero; delete the accepted-cost comment describing a removal path
  that did not exist when written; keep the note that the admission bound does **not** free at
  `Live → Removing` — capacity returns only when the exact row deletion lands.
- [ ] **Gates and commit:** the CA battery under one `flock` hold covering build and gate, both
  object-storage lanes, and commits by explicit path.

#### What died, and why — do not re-add {#t5-died}

- **"Entry deleted last" as the load-bearing invariant.** Unsound: not stable while the entry says
  `Removing` with name-keyed independent cursors. Replaced by opaque life identity plus one catalog-built
  row set, not by another ordering assertion.
- **`RemovalReady`.** It was the correct repair for the old name-keyed, multi-producer design, but its
  only job was to make cursor pruning stable before entry deletion. The new row is keyed by the old
  `life_id`, cannot be inherited by rebirth and is unconditionally omitted by the next catalog-built
  plan. Keeping the state would add an enum value, transition, pruning round, finalizer, CREATE
  assistance and recovery windows without proving a remaining fact.
- **Independent name-keyed cursors and cleanup maps.** The earlier rewrite rejected life-keying because
  it assumed every producer could carry a dead id forever. The single catalog-built plan removes that
  premise: only `Live`/`Removing` ids enter the output key set, so absent rows are
  discarded on every seal, including REBUILD. One life row replaces two wire collections, their join,
  the fictitious shard-zero grammar and five synchronized predicates.
- **Permanent refusal of rebirth, and a permanent `Retired` row.** Both need somewhere to remember every
  retired name, which is unbounded for opaque names; and permanent refusal breaks supported reuse —
  shadow backup names, explicit UUIDs in replicated DDL replay, UUID-less table paths.
- **The fabricated-missing-entry anomaly requirement.** Relocated, not removed: a legal removal and a
  fabricated entry loss are byte-identical, so it is pinned at the mutation API instead (step 2).
- **The `_cleanup`-driven `Pending → Completed` handshake and the one-shot removal cleanup pass.** With physical cleanup leak-only, the
  marker proved no safety fact and its removal otherwise left promotion ownerless. The terminal fold
  now attaches positive cleanup evidence to the life row directly; its state enum and wire field die.
  A special physical attempt was also rejected: before fold it has no durable suppression verdict, and
  after adoption it can become a stale name-scoped actor across rebirth. The perpetual janitor and
  orphan-manifest sweep already own the leak-only residue.
- **The Σ-index-set exactness decision.** Superseded by one worst-form life row per catalog entry plus
  explicit over-coverage for `btr`/`cnd`. The old cursor-plus-`nsc` combination is no longer a state the
  codec can express.

### Task 5b: `chooseRecoveryGrounding` — recovery becomes LIST-independent {#task-5b}

[Directive design change 3 + implementation improvement 2 = amendment commit 6. **This is the task
that kills `[RECOVER-REF-TABLE-LIST-RESIDUAL]`** — the BACKLOG residual Task 0 Step 3 registered as a
Task 7b PRECONDITION, guarded by two capstone sentinels deliberately written to go RED when it dies.
The terminal-evidence seal remains authoritative until the next invocation's pre-fold drain, but that
interval has no recovery finalizer and no fourth lifecycle state. This task and Task 7 therefore share
only the ordinary `Removing` recovery contract; neither may perform or bypass the catalog drain.]

**Files — THERE ARE TWO RECOVERY ENTRY POINTS AND THE DIRECTIVE BINDS BOTH.** They have confusingly
similar names, they are not related by code, and only one of them is where the `hint_log_ids`
discussion applies. An implementer who touches one and reports "done" has done half the task:
- Modify: `.../Pool/CasRefLedger.cpp` — `runRecoveryWalkOnce` (`:532`), the LIVE WRITER MOUNT's
  spec-§4 walk: `hint_log_ids` declared `:554`, filled `:579`, sorted `:591`; **the genesis fallback
  `else if (!hint_log_ids.empty()) walk_from = RefTxnId{hint_log_ids.front().writer_epoch, 1};`
  (`:653-654`) is DELETED**; the `_ckpt.life_epoch` grounding (`:651-652`) becomes the ONLY genesis
  source; the hint's remaining role (`:669`) is exact-key fetch plus diagnostics; the seal-grammar
  contextual check (`:788-794`) already refuses to fabricate `life_epoch` and stays;
  `ensureRefTableRecovered` (`CasRefLedger.h:700`, def `:921`) and `installRecoveryResult` (`:1150`)
  carry the lifecycle gates
- Modify: `.../Pool/CasRefProtocol.cpp` — `recoverRefTable` (`:976`, declared
  `CasRefProtocol.h:803-805`) and the real body `recoverRefTableDetailed` (`:889-974`), used by
  fsck / GC / the orphan sweep. **This is the one the BACKLOG residual is named after, and today it
  is ENTIRELY LIST-DRIVEN: one LIST, newest snapshot, tail replay, restart-on-vanish — no
  `hint_log_ids`, no `_ckpt`, no `life_epoch`, no catalog.** Making it LIST-independent is the
  substantial half of this task: it must take the grounding from `chooseRecoveryGrounding` like the
  writer path does, which means it needs the catalog entry and `_ckpt` passed in (the callers have
  both after Task 4). Its "one LIST" survives only as a snapshot-candidate offer and a diagnostic
  witness source
- Modify: `.../Pool/CasRefCkpt.h`/`.cpp` if the pure helper lands beside `mergeCkpt` (implementer's
  call; it must be reachable from a TU with no backend)
- Modify: `docs/superpowers/cas/BACKLOG.md` — `[RECOVER-REF-TABLE-LIST-RESIDUAL]` → CLOSED, naming
  this commit
- Modify: `src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp` — the two capstone sentinels
  (regions `:511`, `:561`) go red BY DESIGN here; adapt them per their own failure text (this is one
  of the amendment's two legitimate edits of that file; Task 7b owns the other)
- Create: `src/Disks/tests/gtest_cas_recovery_grounding.cpp`

**Interfaces:**
```cpp
/// Pure: no backend, no LIST, no clock. `greatest_hinted_snapshot` is a HINT-derived candidate —
/// it may only RAISE the base; it may never supply genesis.
struct RecoveryGrounding
{
    std::optional<RefTxnId> base;   /// none => this life has no committed base yet
    RefTxnId walk_from;             /// base ? successor(base) : {life_epoch, 1}
};

RecoveryGrounding chooseRecoveryGrounding(const CatalogEntry & catalog_state,
                                          const std::optional<RefCkpt> & ckpt,
                                          const std::optional<RefTxnId> & greatest_hinted_snapshot);
```
- Grounding rules, verbatim: "choose the greater checkpoint/hinted snapshot as the base; with a base,
  walk from its successor; without a base, walk from `{life_epoch, 1}`; never derive genesis from log
  LIST results; fail closed when the lifecycle requires information that is absent."
- Lifecycle rules, verbatim: "`Creating` namespaces are never recovered or published; `Live` and
  `Removing` namespaces require a readable `_ckpt` with `life_epoch`; a missing required `_ckpt` in
  either state is corruption. An id absent from the catalog is not recovered."
- What LIST may STILL do, verbatim: "offer a newer snapshot candidate; provide additional diagnostic
  witnesses; nominate garbage for cleanup." The hot hint is Task 4d's one
  `LIST(cas/ns/stream/)`, which deliberately includes immutable `_log` and `_snap` but excludes
  `_ckpt` and `_files`. What it may not: "determine genesis or committed history."
- **Honest finding, already true at Stage A:** the directive's "Never fabricate `life_epoch` with
  `value_or`" is ALREADY satisfied on the writer path — `CasRefLedger.cpp:646` and `:788` carry
  explicit comments saying the omission is deliberate. This task therefore VERIFIES and FENCES it (a
  test + keeping those comments), and the report says so rather than claiming a fix that was not
  needed. It is NOT satisfied on the `recoverRefTableDetailed` path, which has no `life_epoch`
  concept at all — there the requirement is new construction, not a fence.

- [ ] **Step 1: the LIST audit, BEFORE any code change** (directive: "Audit every remaining use of
  recovery LIST data"). Produce a table in the task report covering BOTH entry points: every
  recovery-path use of LIST-derived data — the `hint_log_ids` producers (`CasRefLedger.cpp:554-591`),
  the genesis fallback (`:653-654`), the exact-key fetch loop (`:669`), the sampled snapshot
  candidate, `recoverRefTableDetailed`'s entire LIST-driven shape (`CasRefProtocol.cpp:889-974`) and
  each of its callers' expectations, plus anything else the audit finds — each classified as
  CORRECTNESS / performance / diagnostics / leak-only-cleanup. **Stop-and-document rule, verbatim:
  "If any LIST result still affects correctness rather than performance, diagnostics or leak-only
  cleanup, stop and document the unresolved dependency instead of preserving a fallback."** A
  surviving correctness use means this task STOPS, writes the dependency into the report and
  `BACKLOG.md`, and the controller rules — it does not get papered over with a fallback.
- [ ] **Step 2: Failing tests** in `gtest_cas_recovery_grounding.cpp` (pure ones need no backend):
  `RecoveryIsEquivalentUnderFullEmptyPartialAndReorderedList` — the directive's recovery-equivalence
  test: for the SAME exact objects, four LIST behaviours (complete / empty / partial / reordered)
  reconstruct an identical logical state — assert the installed table state, `last_epoch_seal` and
  next id are equal across all four; only request counts, diagnostics and discovered garbage may
  differ (assert THOSE are allowed to differ, so the test cannot be satisfied by making LIST
  irrelevant to performance too);
  `LiveWithoutReadableCkptIsCorruption` (the directive's "missing `_ckpt` for `Live`" test);
  `LiveWithCkptLackingLifeEpochIsCorruption`;
  `RemovingWithoutCkptIsCorruption`;
  `AbsentLifeIsNotRecoveredEvenWhenCkptSurvives` (Task 5's direct deletion leaves checkpoint cleanup to
  the janitor);
  `CreatingIsNeverRecoveredOrPublished`;
  `GenesisNeverComesFromHintedLogIds` — the deleted fallback, stated as a behaviour: hint offers
  `{E,1}` while `_ckpt` has no `life_epoch` → corruption, NOT a walk from the hint;
  `HintedSnapshotMayOnlyRaiseTheBase` — a hinted snapshot below `_ckpt.checkpoint` is ignored, above
  it is adopted;
  `LifeEpochIsNeverFabricated` — the `value_or` fence described above.
- [ ] **Step 2b: the capstone sentinels and the residual.** Run
  `gtest_cas_list_liar_end_to_end.cpp` — the two sentinels (`:511`, `:561`) MUST now be red; that is
  the designed signal, not a regression. Adapt them per the fix instruction in their own failure
  messages, flip `[RECOVER-REF-TABLE-LIST-RESIDUAL]` to CLOSED in `BACKLOG.md`, and record in the
  report that Task 7b's precondition is discharged (Task 0 Step 3 was written to refuse until
  exactly this).
- [ ] **Step 3:** → FAIL. **Step 4:** Implement (extract the helper first, then delete the fallback —
  the deletion is the behaviour change and belongs in one reviewable step). **Step 5:** Full CA gate
  + both CA-s3 lanes green. **Step 6: Commit**
  `ca: ref — LIST-independent recovery: chooseRecoveryGrounding, no hint-derived genesis`.

### Task 6: Read-side contract — refs AND namespace files {#task-6}

**AND THIS TASK IS WHERE THE LIFE-RESOLUTION REFACTOR BELONGS (placed 2026-07-30, correcting an earlier note
of mine that said it should be its own task).** Deleting `stageATransition` already forces a decision at
every one of its sites about what to do when no catalog life is known — so the decisions are this task's
whether or not the refactor is named. Do them with the mechanism rather than by substituting a new default:

- [ ] **Make absence expressible in the type.** `CasRefCatalog::resolveLifeOrSentinel` returns a
  `NamespaceLifeId` and falls back to the sentinel, so a caller **cannot distinguish "here is the life" from
  "I do not know"** — it gets a plausible, well-formed, wrong key. That property is the amplifier in all three
  vacuous-frontier findings (`{#r11-empty-universe-vacuous}`, `{#r11b-authority-vs-union}`,
  `{#r11c-incarnation-mismatch}`). Return `std::optional`, delete the fallback, and let the compiler enumerate
  the sites. Fixtures then ask for the sentinel explicitly, where it IS the truth.
- [ ] **Pass the snapshot, do not re-read it.** `resolveLifeOrSentinel` calls `CasRefCatalog::read` — a full
  pool-wide GET and decode — on **every** call, at 24 sites, several inside per-namespace loops. Task 5
  establishes one complete `CasRefCatalog::Snapshot` per GC/REBUILD run and replaces `FoldResult`'s lossy
  `live_incarnation` projection. Thread that same one-read seam through the remaining fsck,
  decommission and reader operations while deleting `resolveLifeOrSentinel`. Two defects disappear
  rather than get fixed: the O(namespaces²) read volume, and "two resolutions inside one operation
  disagreed" becomes unrepresentable. Preserve the full state: absent, `Creating` and a different
  incarnation have distinct intake/lifecycle meanings. An absent canonical life is inert debris and
  must not raise the old pool-suppressing damage anomaly; an input ref-life row for it is Task 5's
  separate alert-and-discard diagnostic.
- [ ] **Collapse the fixture/production divergence into ONE named seam.** Raw helpers write at the sentinel,
  admit catalog entries as `Live` with no `_ckpt`, and bypass birth — a habit spread across files rather than a
  decision made once. It produced the 164-test sweep, the test whose premise a uniform pin inverted, and two
  tests that pinned data loss as correct. One helper, one documented list of divergences, one place to look. This
  task is where it lands because removing the sentinel already rewrites those helpers.
- [ ] **Consider giving the sentinel its OWN type** (or the life a provenance tag) rather than deleting it by
  grep. Its danger is that it is the same type as the truth, which is why it can be silently substituted; a
  distinct type makes "this function requires a catalog-derived life" a compile-time property, and makes this
  task's own zero-grep gate mechanical — delete the type, and every remaining use is a build error.

**NEW CALL SITE, added after this task's brief was written (2026-07-30).** Task 4-C adds a
`casAdmitEntry` inside `writeRefLogTxnRaw` (`src/Disks/tests/cas_test_helpers.h`) keyed at
`NamespaceLifeId::stageATransition(ns)`'s sentinel, so the 164 raw-fixture tests keep matching the keys
they already write. That is a deliberate bridge, and **this task owns its removal**: `stageATransition`
remains for the reader paths until this task replumbs them, and its deletion plus the tree-wide zero-grep
gate live here. Task 4-C does not close it and must not claim to. Count the sites yourself when you get
here — do not trust any figure written earlier.

**And know what the fixtures' catalog entries deliberately are NOT.** That admission writes `state = Live`
with no `_ckpt`, whereas production reaches `Live` only through `completeCreation`, which publishes `_ckpt`
first (INV-4). This is not an oversight to fix: several fixtures exist specifically to build **a table with
no `_ckpt`**, so forcing one would destroy what they test. `Gc::readCheckpointWitnesses` tolerates an absent
`_ckpt` (only a present-but-undecodable one is held), which is why it costs nothing today. If you ever add
code that assumes `Live` implies a `_ckpt`, these fixtures are where it will break, and the assumption —
not the fixtures — is what would be wrong for tests. These `Live`-without-`_ckpt` fixtures are
test-only seams and must never enter production recovery, where `Live` and `Removing` require a
readable `_ckpt`.


[Amendment commit 7's read/write half; the cleanup half is Task 5. This task owns the LIFE HANDLE
itself, which is why the namespace-file hot-path requirements land here and not in Task 4b: "hot
reads and writes use an already-held life handle" needs the handle to exist.]

**Files:**
- Modify: the ref read paths (`.../Pool/CasRefLedger.cpp` readers; the table-cache layer —
  `RefTableCacheEviction*` test family marks the surface), destructive cleanup sites from
  Stage A Task 9's list
- Modify (amendment, namespace files): `.../Pool/CasPlainObjects.cpp` hot read/write paths take the
  life from the caller's handle; `ContentAddressedMetadataStorage.cpp` — delete
  `namespaceFilesReadable` (`:1231`, body `return !store()->namespaceIsRemoved(ns);`) and rewire its
  five call sites (`:1325` in
  `existsFile`, `:1498` in `existsDirectory`, `:1647` and `:1695` in `listDirectory`, `:1846` in
  `tryGetInManifestBytes`); `ContentAddressedTransaction.cpp` — the DELAYED-WRITE seam: `writeFile`
  (`:793`) constructs a `CaInlineWriteBuffer` (`ContentAddressedTransaction.h:398`, `OnInlined`
  `:401`, `finalizeImpl` `:414`, def `:1899-1903`) whose verbatim-file branch (`:811-824`) captures
  `[this, ns, name, carried = …]` at `:820` and calls `putNamespaceFile(ns, name, carried + bytes)` at
  `:822` LATER, when finalize fires
- Create: `src/Disks/tests/gtest_cas_ref_read_contract.cpp`,
  `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp`

**Interfaces (§2 verbatim):**
- Live readers hold `NamespaceLifeId` handles and can never alias a new life (foreign prefix);
  a stale reader gets stale-or-`NotFound`, NEVER rejection (no fence/catalog read on hot
  paths — the read side pays zero new requests).
- Destructive cleanup of a CURRENT catalog life revalidates life and fence immediately before every
  per-key `deleteExact`; a catalog transition can change what that key means between two deletes.
  Task 5's dead-life janitor is the explicit exception proved by stronger ordering: it takes a fresh
  catalog cut AFTER each LIST page, and catalog-before-object publication plus non-reused `life_id`
  protects every candidate already present in that page. It still checks the GC fence and uses
  exact-token delete per key, but it does not perform O(keys) catalog GETs. Do not generalize that
  page-cut optimization to `cleanupRefObjects`, whose target is still a current life.

**Interfaces (amendment — namespace-file implementation requirements, directive verbatim except the
annotations):**
- "Every namespace-file API accepts `NamespaceLifeId`" (landed in Task 1c/4b; asserted here).
- "Hot reads and writes use an already-held life handle; they must not issue a catalog GET"
  (Constraint 16 — a catalog GET per file operation is exactly the insert-path cost the directive
  forbids).
- "Delayed write-buffer callbacks capture the exact incarnation present at admission" — concretely,
  `ContentAddressedTransaction.cpp:820`'s lambda captures a `NamespaceLifeId`, NOT a `RootNamespace`
  to be re-resolved when `finalizeImpl` runs. This is the one place where the type system does not
  help by itself: capturing the bare name still compiles.
- "A stale reader may return stale data or `NotFound`, but never data from a newer incarnation."
- "A stale writer may only target its old incarnation and must never write into the new one."
- **`namespaceFilesReadable`'s disposition, decided here and recorded by Task 5.** Delete it with the
  `Removed` snapshot. Fresh name resolution already returns no handle for `Removing` or absent rows; a
  hot path that already holds the old `NamespaceLifeId` follows the explicit
  stale-or-`NotFound` contract and must not pay a catalog request merely to force absence. Re-keying
  prevents that handle from ever resolving into a newer incarnation. Keeping the gate would either
  retain a dead snapshot solely for itself or violate the zero-catalog-request hot-path rule.

- [ ] **Step 1: Failing tests**: reader holding inc₁'s handle after drop+rebirth reads
  stale-or-`NotFound`, never inc₂ data (the alias test); hot read path performs ZERO catalog
  requests (op-count assert); entry token changed between plan and the FIRST delete → nothing
  deleted; token changed BETWEEN two keys of one cleanup pass → the first key's delete lands,
  the second is refused (the per-key revalidation race — codex finding 14).
- [ ] **Step 1b: the namespace-file half**, in `gtest_cas_ns_file_read_contract.cpp` — the
  directive's three remaining required tests:
  `StaleReaderAfterRebirthNeverSeesNewIncarnation` — a reader holding inc₁ reads a file name that
  exists in BOTH lives with different bytes: it gets inc₁'s bytes or `NotFound`, never inc₂'s;
  `DelayedWriterFinalizedAfterRebirthWritesOnlyItsOwnIncarnation` — open a `CaInlineWriteBuffer` under
  inc₁, drop and recreate the namespace as inc₂, THEN finalize: the bytes land under inc₁ (or the
  write fails), and inc₂'s object at the same relative name is untouched — the test that catches a
  lambda capturing the bare namespace;
  `NamespaceFileHotPathsIssueZeroCatalogRequests` — read, write, remove and list a namespace file
  through a held handle; the op journal shows ZERO catalog GETs (the directive's "zero catalog
  requests on namespace-file hot paths" test, and the read-side half of Constraint 16).
- [ ] **Step 1c: Retire the transition constant** [codex r3 finding 1, widened by the amendment]:
  with the reader paths AND the namespace-file paths replumbed to handles, DELETE
  `NamespaceLifeId::stageATransition()` (Task 1 shipped it at `CasRefNamespaceId.h:100-108`, its
  sentinel incarnation spelling `__STAGE_A_TRANS` in hex); gate: a tree-wide grep for
  `stageATransition` returns zero build-input hits, recorded in the report. This gate now covers the
  file call sites Task 1c introduced — if any remain, this task is not done.
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate green + the lane that exercises
  dedup-log rotation (the write side of Constraint 16 must not regress here either).
- [ ] **Step 5: Commit** `ca: ref — read-side contract: handle-scoped reads and namespace files, pre-delete life revalidation`.

### Task 6b: `trySnapshotPublishOnce` says what it does {#task-6b}

[Directive implementation improvement 3 = amendment commit 8, and the last of the series. A naming
change with a hard ordering obligation attached.]

**Files:**
- Modify: `.../Pool/CasRefLedger.h` (`:190` declaration) and `.../Pool/CasRefLedger.cpp` (`:3440`
  definition; the internal call from `dispatchSnapshotPublisher` at `:3242`; the comment references at
  `:451`, `:2218`, `:3229`, `:3401`, `:3600`)
- Modify: `.../Pool/CasPool.h` (`:540` declaration, `:797` comment) and `.../Pool/CasPool.cpp`
  (`:1564` forwarder) — the public surface renames with it
- Modify: any test naming it (grep before starting; the rename must not silently drop coverage)
- Create (only if Step 1 finds the ordering unasserted):
  `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp`

**Interfaces:**
- New name: `tryPublishSnapshotAndAdvanceCheckpointOnce` (the directive's own suggestion). **RENAME,
  not a split of the public method, unless the implementer finds a split that keeps both effects in
  ONE retry unit:** the two durable effects must not become separately callable, or a caller could
  advance `_ckpt` without a durable body. Either shape is acceptable to the directive ("Rename or
  split"); the name must say both effects in either case, and the decision goes in the report.
- Required ordering, PRESERVED verbatim: "1. immutable snapshot body becomes durable; 2. `_ckpt`
  advances; 3. the new snapshot is adopted in memory." Today: body bytes sealed at `:3546`, key at
  `:3557`, `PUT` at `:3559`; `_ckpt` advance via `publishCkptContribution(… .checkpoint_snapshot_id =
  candidate_x …)` at `:3592-3595`; in-memory adoption under `state_mutex` at `:3616-3648`
  (`newest_snapshot_id` `:3644`, `base_snapshot_bytes` `:3647`).
- "Do not change its retry/backoff semantics" — the admission gate `admitSnapshotPublishUnderStateLock`
  (`:3192`, backoff check `:3206`), `advancePublishBackoff` (`:3314`), `resetPublishBackoff` (`:3326`),
  and the `maybeScheduleSnapshotPublish` (`:3294`) → `dispatchSnapshotPublisher` (`:3226`) →
  `settleSnapshotPublish` (`:3267`) loop are untouched.
- `Poisoned` keeps blocking publication (Constraint 17: it means a durable transaction may be missing
  from the cached view, so it must continue to block publication and trigger re-recovery) — the
  refusals at `:451` and `:2218` survive the rename with their comments corrected to the new name.

- [ ] **Step 1:** Locate the EXISTING ordering coverage first and record where it lives. A rename
  must not be the change that quietly loses an assertion. If the three-step ordering is not asserted
  anywhere, add it in `gtest_cas_ref_snapshot_publish_ordering.cpp` before renaming:
  `SnapshotBodyIsDurableBeforeCheckpointAdvances` (backend op journal order),
  `AdoptionHappensLastAndOnlyAfterBothDurableEffects`,
  `PoisonedRefusesPublicationAndTriggersReRecovery`,
  `RetryBackoffUnchangedAcrossRename` (the admission gate's decisions for a fixed clock sequence are
  identical before and after — capture the before-values as literals).
- [ ] **Step 2:** Rename (or split under the constraint above). **Step 3:** Full CA gate green — a
  pure rename must move no test count. **Step 4: Commit**
  `ca: ref — tryPublishSnapshotAndAdvanceCheckpointOnce: the name states both durable effects`.

### Task 7: R5 — decommission duties {#task-7}

**Carried code residues from the Task 1c review** (prose findings from that review went to
`docs/superpowers/cas/deferred-docs-fixes.md` instead; these three are executing defects, so they
stay here and keep their review):

- [ ] **The decommission test's assertion does not establish what its comment claims.**
  `gtest_cas_decommission.cpp`, `CasDecommission.LifelessKeyRefusesTheWholeCommandFailClose` asserts
  a broad listed prefix under the comment "the healthy namespace's refs are untouched" — but the
  planted lifeless key itself satisfies that assertion. After Task 4d no victim-name physical prefix
  exists at all. Assert the healthy row remains in the exact catalog cut and its known stream/state
  keys retain their exact tokens, or assert `report.namespaces_removed`; never replace the old check
  with a scan for `victim` text inside opaque-id paths.
- [ ] **The two path-parser residues die in Task 4d, and Task 7 verifies their absence.** The vestigial
  empty-namespace guard in `Pool::listNamespaces` and the loose pre-flight LIST base that matched
  `victim2` both depended on deriving ownership from path-shaped life keys. Task 4d deletes that
  discovery route. Gate this task with a source/test inventory showing decommission selects the victim
  exclusively by exact catalog-name ownership and has no life-key prefix fallback.

**Files:**
- Modify: `.../ContentAddressed/Tools/CasDecommission.cpp` (scoped-LIST discovery `~:116`
  replaced) [path per codex finding 18]
- Create: `src/Disks/tests/gtest_cas_decommission_catalog_duties.cpp`

**Interfaces (register R5 verbatim — same-rollout dependency of the catalog):**
After claiming the victim server root: enumerate its catalog entries EXACTLY (no LIST);
`Removing` without a terminal record = resumable writer work — `_ckpt` present → recover and
append the terminal under the claimed fence; `_ckpt` absent while still `Removing` = corruption,
because Task 5 has no path that deletes a checkpoint while its catalog row exists. A `Removing` row
whose terminal has folded remains GC-owned until GC performs the proved exact row deletion;
decommission wakes GC but does not add a second deletion driver. The next GC invocation drains it
before DEFER or successor adoption. Once the row is absent, surviving
opaque `_ckpt` or other life objects are janitor debris, not owned namespace work. Perform a FINAL exact catalog GET/token check
immediately before slot retirement; retirement is FORBIDDEN while any entry owned by that root
remains.

[Amendment note: opaque ids remove the former checkpoint-finalization windows. For a root that never
returns, decommission still recovers unfinished `Removing` writer work under its claimed fence, while
GC remains the sole actor that deletes a proved complete row. It must neither re-create a missing
`_ckpt` nor perform a fallback catalog deletion.]

- [ ] **Step 1: Failing tests**: hidden `Removing` entry (LIST would have missed it; catalog
  does not) blocks slot retirement; the `_ckpt`-present resumption appends the terminal under
  the claimed fence; `Removing` without `_ckpt` is corruption and remains owned;
  a proved-complete `Removing` row remains owned and wakes GC rather than being deleted by decommission;
  after GC deletes it, surviving `_ckpt` bytes do not count as catalog-owned work;
  the token-changed-at-final-check race → retirement refused, retried; retirement with zero owned
  entries proceeds. The recovery tests use a dead root, not a mounted writer helper.
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate +
  `test_content_addressed_drop_pool_member` lane green.
- [ ] **Step 5: Commit** `ca: decommission — catalog-exact duties; retirement fenced on owned entries`.

### Task 7a: delete probe A — the second full stream LIST goes {#task-7a}

[2026-07-30 GC directive step 1: "Удалить probe A. Удалить дополнительный LIST, setting, counters,
phase, tests и устаревшие комментарии. Сохранить B1/B2 и mount-time capability probe."
**Rationale — why it goes rather than stays:** probe A is a sampled store-quality detector whose
entire signal is "a LIST can be a liar". Under a catalog-authoritative universe (Task 4) and
LIST-independent recovery (Task 5b), no correctness decision rests on LIST fidelity any more — the
detector measures a property nothing depends on, and it pays one extra FULL enumeration of
`cas/ns/stream/` per sampled round to do it. Register R7 ("Probe A gating policy — DECIDED and EXECUTED")
is superseded by this task, and its supersession note lands in the same commit. **This is its own
commit, deliberately separate from Task 7b:** probe-A removal is a PERFORMANCE change and destruction
enablement is a CORRECTNESS change — "так легче отличить performance effect от возможной
correctness-регрессии" (Constraint 18).]

**Files:**
- Modify: `.../Gc/CasGc.h` — DELETE `sampleRefListQuality` (`:668`) and its two documentation blocks
  (`:206` the gating verdict, `:657-667` the detector's contract)
- Modify: `.../Gc/CasGc.cpp` — DELETE the definition (`:3522-3709`, including its
  `GcPhaseTimer t(phase_sink, "ref_list_probe")` at `:3549`, the setting read at `:3550`, the five
  ProfileEvents increments at `:3562`/`:3576`/`:3640-3641`/`:3699`/`:3702`, and the per-hole
  `gc_anomaly` audit emission with `ev.detail = {{"probe","A"}, …}` at `:3647-3663`), the single call
  site (`:460`, `sampleRefListQuality(ref_scan, new_round)`) and its comment (`:456-459`); update the
  stale comment at `:1351`. **The phases are NOT an enum — they are ordered `const char *` literals
  numbered only in comments as `PHASE N/19`, so deleting phase 4 means renumbering phases 5-19 down to
  4-18 and changing every `/19` to `/18`** (the phase list, in order: `lease` `:293`,
  `heartbeat_floor` `:338`, `defer_decision` `:394`, **`ref_list_probe` `:456` ← deleted**,
  `parent_seal_read` `:478`, `fold_ref_group` `:1355`, `fold_seal_read` `:1399`, `fold_ref_intake`
  `:1620`, `fold_ns_cleanup_scan` `:2532`, `fold_reduce` `:2677`, `fold_seal_write` `:2908`,
  `pending_deletes` `:519`, `meta_pool_wait` `:749`, `round_commit` `:776`, `handoff_reclaim` `:839`,
  `manifest_deletes` `:893`, `namespace_cleanup` `:942`, `ref_object_cleanup` `:955`, `orphan_sweep`
  `:968`). A stale `N/19` is the kind of thing that survives for a year.
- Also stale-comment sites, five of them, none inside the deleted block: `Gc/CasGc.h:206`
  (`RefScanSummary` doc), `Gc/CasGc.cpp:1226` (`newestFoldSealRef` "in the same spirit as probe A"),
  `Gc/CasGc.cpp:1558` (**the B1 doc — "probe A covers the listing, probe B2 covers everything below the
  intake"; B1 STAYS, only the probe-A half of the sentence goes**),
  `src/Disks/tests/cas_test_helpers.h:1272` (`HintHoleBackend` doc),
  `src/Disks/tests/gtest_cas_gc_arithmetic_intake.cpp:48`
- Modify: `.../Pool/CasPool.h` — DELETE `PoolConfig::gc_probe_a_period` (`:119`, default 16) and its
  doc block (`:109-118`). **It is a `PoolConfig` struct field only, NOT a user-facing setting** — no
  `DECLARE(...)` entry in `ContentAddressedSettings.cpp` the way `gc_shards` and
  `gc_snap_generations_to_keep` have, and no XML/DDL/config-template binding anywhere in the tree. So
  there is nothing to remove from the settings surface and no config-compat question; verify with the
  Step-1 inventory rather than assuming either way
- Modify: `utils/ca-soak/soak/signals.py` (`:53-85`) — **a LIVE consumer, not a comment**: the soak
  harness watches `CasGcProbeAHolePresent`/`HoleAbsent`/`Due`/`Performed` BY NAME. Left alone it
  silently watches counters that no longer exist, which is worse than a build error because the soak
  keeps reporting green. This is the one non-C++ code dependency of the deletion
- Modify: `src/Common/ProfileEvents.cpp` — DELETE the SIX events at `:885-890`:
  `CasGcRefScanDisagreements` (probe-A-only: "taken by a SAMPLED GC round"), `CasGcProbeADue`,
  `CasGcProbeAPerformed`, `CasGcProbeASkipped`, `CasGcProbeAHolePresent`, `CasGcProbeAHoleAbsent`
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp` — DELETE the `ref_list_probe`
  phase from the enum and from the execution-order column comment (`:60`), and the
  `due`/`performed`/`skipped`/`holes` example from the phase-metrics column comment (`:64`). The
  generic phase plumbing itself is untouched: `GcPhaseTimer` → `GcPhaseRecord` → `Gc::phase_sink` →
  `CasGcScheduler.cpp:154-158` → `makeGcRoundLogger` (`ContentAddressedMetadataStorage.cpp:455-510`)
  serves all phases and only loses one row kind
- Modify: `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` — **DELETE
  the `ref_list_probe` row (`:75`)**, whose cost column literally reads "one full ref-prefix `LIST` on
  a due round, none otherwise". This is a USER-FACING documented phase of
  `system.content_addressed_garbage_collection_log`, so the doc and the enum must change together in
  this commit; a phase documented but never emitted is worse than either
- Delete: `src/Disks/tests/gtest_cas_holey_list_detector.cpp` (389 lines, 3 `TEST`s — the detector's
  own file). Verify all three are probe-A-only before deleting the file rather than the tests
- Modify: `src/Disks/tests/gtest_cas_retirement_sweep.cpp` — THREE probe-A tests here, and they do
  NOT all get the same treatment (verify names/anchors at execution; the file's "item 1: probe A,
  demoted" section starts `:240`):
  (i) `ProbeAReportsAHintHoleAndTheRoundFoldsThroughItAnyway` (`:249`) — DELETE;
  (ii) `TheDetectorsCadenceIsOnEveryFoldingRoundsRow` (`:421`) — DELETE (there is no cadence left);
  (iii) **`TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond` (`:384`) — CONVERT, DO NOT
  DELETE.** It already counts ref-prefix enumerations per round and varies `gc_probe_a_period`
  (`:392`), including the `= 0` disable assertion (`:415`) — it is the existing proof of the
  two-LIST claim, which makes it the natural home for Step 3's criterion and the before/after
  anchor Task 12 needs. Rewrite it to assert ONE enumeration unconditionally and rename it
  accordingly (e.g. `TheRoundEnumeratesTheRefPrefixExactlyOnce`). Deleting it would discard the only
  test that ever measured what this task claims to improve
- Modify: `src/Disks/tests/gtest_cas_gc_log.cpp` — the phase-order expectation list (`:383`) and the
  `metricsOf(rows, 0, "ref_list_probe")` assertion (`:413`)
- Modify: `docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md` — R7's supersession note
  (**already written 2026-07-30 with the plan amendment; verify it still matches what was deleted**)
- Modify: `docs/superpowers/cas/todo-20260726.md` — **item 1 (near `:110`) records an OLD proposal to
  "Remove probe B1 — self-declared blind to the suspected defect". The directive says "Сохранить
  B1/B2", so that proposal is OVERRIDDEN and must be marked so.** Left as-is it reads like standing
  intent, and the next reader deletes B1 believing it was agreed. Its probe-A paragraph (`:120`) is
  also now history
- Modify (doc sweep, same commit — the deletion is not done while the tree still describes the
  detector as live): `docs/superpowers/cas/BACKLOG.md` (`:167`),
  `docs/superpowers/cas/2026-07-28-stage-a-RESULTS.md` (several),
  `docs/superpowers/cas/2026-07-28-stage-a-retirement-verdicts.md` (`:74` — the verdict that DEMOTED
  probe A; it becomes the history of a deleted detector, not a live policy),
  `docs/superpowers/cas/11-walkthrough.md` (`:1879`)
- Modify: `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md` — §5's "Probe A:
  sampled, deterministic cadence, durable due/performed/skipped observability; aborts nothing; the
  mount-time store gate (#23) is separate." That sentence is accurate TODAY and false the moment this
  task lands; it is corrected HERE, not pre-emptively

**KEEP — named so the deletion cannot over-reach:**
- **B1 and B2 accounting.** B1 is `logs_accounted == logs_applied` on the `fold_ref_intake` phase row;
  B2 is the ordinals/`produced=false` accounting. Neither has anything to do with the detector.
- **The mount-time capability probe (#23):** `Backend/CasProbe.h` (the capability battery under the
  reserved `<prefix>/_probe/` subtree) and `Backend/CasSentinelProbe.h`. Confirmed DISTINCT code from
  probe A — different file, different purpose, different lifecycle.
- **FALSE POSITIVES a name-grep will hand you — do not touch them:**
  `gtest_cas_upload_fanout.cpp:914`'s `probe_acquired` (an upload-permit test) and
  `gtest_cas_gc_shard_plan.cpp:236`'s local `probe_a` (a `ManifestId` variable). Neither is the
  detector.

- [ ] **Step 1: the removal, red-first in the only sense available.** A deletion has no failing test
  to write, so the pre-condition is an INVENTORY: grep `probe_a|ProbeA|ref_list_probe|
  sampleRefListQuality|RefScanDisagreements` across the whole tree (source, tests, configs, docs) and
  paste the full hit list into the task report. Every hit is then either deleted, corrected, or
  explicitly classified as a false positive with the reason. The report's after-grep must return only
  the false positives.
- [ ] **Step 2: dead data plumbing goes too** (Constraint 3 — no leftovers whose only consumer left):
  inspect `RefScanSummary` for fields the detector was the sole consumer of, and delete those with it.
  A struct that still carries a field nobody reads is the residue this step exists to prevent.
  **And the enumeration split itself:** today `Gc::enumerateRefPrefix` (`CasGc.cpp:3462`) has exactly
  two callers — `Gc::listRefPrefix` (`:3501`, "the round's ONE full enumeration of `cas/ns/stream/`",
  comment at `:390`) and the detector's `probe_scan = enumerateRefPrefix()` (`:3570`). Task 4 already
  replaced `discoverUniverse`'s enumeration with one catalog `GET`, so after this deletion the helper
  has a single caller. Decide and record: collapse it into `listRefPrefix` if the split existed only
  to give the detector a second, independent enumeration, or keep it with a comment saying what else
  justifies it. Do not leave the question unasked.
- [ ] **Step 3: the result criterion, asserted** (directive: "после удаления probe A нет второго
  полного ref LIST"): the CONVERTED test (iii) above asserts a folding round performs EXACTLY ONE full
  enumeration of `cas/ns/stream/` — and, critically, on EVERY round including the ones that used to be
  sampled (with the old cadence, round 16 would have been the one to catch this; a test that only
  checks round 1 proves nothing). Assert by LIST count attributed to the stream prefix, not by wall time.
  The claim is verifiable rather than merely plausible because `Gc::enumerateRefPrefix`
  (`CasGc.cpp:3462`) has exactly two callers tree-wide — `listRefPrefix` (`:3505`, the round's own
  scan) and the detector (`:3570`) — and the round's other `backend.list` calls (`:3004`, `:3147`,
  `:3263`) target manifest, generation or the separately paced `cas/ns/` janitor scan, not the hot
  stream prefix. Re-verify that call-site count in
  Step 1's inventory: if a third caller has appeared, the criterion needs re-derivation, not a pass.
- [ ] **Step 4: Full CA gate — EXPECT THE COUNT TO GO DOWN**, and record the exact expected delta in
  the report before running: 3 tests + 1 suite from the deleted detector file, plus the tests removed
  from `gtest_cas_retirement_sweep.cpp`. A gate comparison that only ever tolerates growth would read
  this as a regression; Task 11's baseline comparison must use the post-7a number.
- [ ] **Step 5: Commit** `ca: gc — delete probe A: no second full ref LIST per round` (+ the R7
  supersession note and the spec §5 correction in the same commit — the register and the spec must
  never describe a detector the code no longer has).

### Task 7b: Destruction enablement — `UniversePolicy::kDefault` → authoritative {#task-7b}

**Files:**
- Modify: the `UniversePolicy` enum's translation unit (`Gc/CasGc.h` region — Stage A Task 9's
  seam) — `kDefault` changes from `StageA_Suppressed` to the authoritative value; comment
  updated to cite THIS task
- Test: the Stage-A cross-namespace kill-shot test in `gtest_cas_list_liar_end_to_end.cpp`

**MEASURED WARNING — flipping `kDefault` is necessary but almost certainly NOT sufficient, and this is
evidence, not a worry.** `suppress_destructive` is a disjunction; the flip clears only the
`universe_authoritative` half. A CA-local stateless lane on 2026-07-31 shows the OTHER half live in an
ordinary healthy pool with no anomalies and no holds:

- `05007`'s pool: `frontier INCOMPLETE (3155 of 3157 namespace(s) proven)` — 2 unproven.
- `05010`'s pool: `frontier INCOMPLETE (11358 of 11369 namespace(s) proven)` — 11 unproven.

Both rounds report `0 anomaly(ies), 0 held namespace(s)`, so `frontier_proven != frontier_namespaces` is
carrying the suppression on its own. Flip the policy in a pool that still leaves a handful of namespaces
unproven and destruction stays off — the four `broken_tests.yaml` entries would stay red and the task
would look like it had failed for a reason nobody had measured.

- [ ] **Before flipping, find out WHY those namespaces are unproven** — a walk target with no
  `_ckpt` entry, a probe-budget exhaustion, or a genuine gap are three different causes with three
  different fixes, and the round already holds the facts to tell them apart. This is a prerequisite
  step, not a risk note.

**Deferred design work — no namespace-local narrowing in this task.** Every carried hold remains
pool-wide suppression. Blob in-degree is pool-wide, and no ownership-partition proof establishes that
a held namespace cannot own a blob nominated from another namespace. The authoritative gate remains
`anomalies || carried_holds || !frontier_complete`; delete the contrary one-held-namespace reclamation
requirement and its test. A future proposal may narrow this only after it supplies that partition proof.

[Codex r2 finding 1: this flip is deliberately AFTER the removal lifecycle (Task 5), the
read-side/pre-delete contracts (Task 6), and the same-rollout decommission duties (Task 7).]

[Amendment note — preconditions UNCHANGED, one of them now has a scheduled discharger: Task 0 Step 3
registered `[RECOVER-REF-TABLE-LIST-RESIDUAL]` as a 7b precondition, guarded by the two capstone
sentinels in `gtest_cas_list_liar_end_to_end.cpp`. **Task 5b is what kills that residual**, and it
adapts those two sentinels when they go red. So by the time this task runs, that file has already
been edited once in Stage B; this task's kill-shot edit is the second and last. Verify the BACKLOG
entry reads CLOSED (naming Task 5b's commit) before flipping the constant — if it does not, the flip
is premature and this task BLOCKS, exactly as the coupling was designed to make it.]

**Interfaces — the gate formula, VERBATIM from the 2026-07-30 GC directive.** What flipping
`kDefault` must produce in a healthy round:
```
frontier_complete = true
suppress_destructive = false
```
What the gate itself must REMAIN:
```
suppress_destructive =
    anomalies ||
    carried_holds ||
    !frontier_complete
```
"То есть holds, budget exhaustion, incomplete frontier и ошибки по-прежнему запрещают удаления" —
holds, budget exhaustion, an incomplete frontier and errors all still forbid deletion. **The
reconciliation a reader will ask for:** the formula has three terms but names four forbidders because
two of them enter through existing terms — budget exhaustion is precisely how `frontier_complete`
becomes false (spec §5: "Budget exhausted first → cursor advances may seal, all destruction
suppressed"), and errors enter as `anomalies`. So this formula is the Stage-A/§5 gate unchanged, and
this task changes only what `frontier_complete` is ALLOWED to become: catalog-proven true instead of
hard-wired false. Nothing about the gate is weakened here — if the flip requires touching any term of
that expression, the flip is wrong and the task raises instead of landing.

This gate governs physical object reclamation and condemnation. Task 5's catalog-only pre-fold drain
is not a fallback object delete: it performs no physical cleanup and has the separate
positive-evidence/no-hold/exact-row proof. It therefore does not consult `suppress_destructive`; an
unresolved drain obligation instead aborts before DEFER, fold or successor adoption.

Exact anchors, so "unchanged" is checkable rather than asserted: the gate is computed ONCE at
`Gc/CasGc.cpp:2708-2709` — `result.suppress_destructive = !report.anomalies.empty() ||
!carried_holds.empty() || frontier_incomplete;`, under the header comment "THE DESTRUCTIVE GATE"
(`:2685`) — with `frontier_complete = universe_authoritative && result.frontier_proven ==
result.frontier_namespaces` at `:2704` and `universe_authoritative = policy ==
UniversePolicy::AuthoritativeForTest` at `:2703`. The enum is `Gc/CasGc.h:53-63` with `kDefault` at
`:62`, and `Gc::runRegularRound`'s default parameter (`Gc/CasGc.h:325`) is the only production entry —
the scheduler passes no policy at all, which is why this is a one-line flip and not a rewrite.

**One naming consequence the flip forces:** the authoritative enumerator is literally named
`AuthoritativeForTest`. Pointing `kDefault` at it makes the PRODUCTION value's name a lie, and the
next reader will "fix" the obvious mistake of a test-only value being the default. Rename the
enumerator to `Authoritative` as part of this task — a mechanical, compiler-enumerated rename. It is
in scope precisely because leaving it invites a revert.

- [ ] **Step 1:** Change `kDefault` (the source-level flip — no other seam exists). The
  kill-shot test's expectations change: "zero deletes
  because suppressed" becomes "zero deletes because namespace `A` is IN the catalog and its
  frontier is probed" — the same scenario now survives on PROOF, not suppression; the
  explicit-`AuthoritativeForTest` variant collapses into the production case and is removed. This is the ONE intentional Stage-B edit of that Stage-A test (the reviewer expects
  exactly this diff).
- [ ] **Step 1b: assert the formula, both halves.** A healthy round on a catalog universe yields
  `frontier_complete == true` and `suppress_destructive == false` and performs real deletes; and each
  of the three terms independently still suppresses — one anomaly alone, one carried hold alone, one
  budget-exhausted round alone (the `!frontier_complete` arm), each with EVERY delete family inert.
  Assert per-family inertness, not an aggregate delete count: an aggregate zero can hide one family
  that ran while another did not.
- [ ] **Step 2:** Full CA gate + both CA-s3 lanes + `test_content_addressed_gc_s3` green —
  destruction now ACTIVE for the first time on the new universe; watch the delete families'
  metrics in the lane logs (nonzero deletes expected, zero anomalies).
- [ ] **Step 3: Commit** `ca: gc — universe authoritative: production destruction enabled (Stage B)`.
  ONE commit for the flip alone — probe A left in Task 7a's commit, and no delete-side optimization
  rides along (Constraint 18).

- [ ] **Step 4: close out every Stage-A return item, and prove each one green with its real assertion
  intact — not merely no longer suppressed.** `grep -n "STAGE-A RETURN ITEM"` finds all five sites; the
  comment at `UniversePolicy::kDefault` names them. Two different things need to happen here, because
  the five sites are no longer in the same state:

  - `05008_ca_gc_snap_prune.sh`, `04290_content_addressed_no_leftovers.sh` and
    `04295_content_addressed_mutation_no_leftovers.sh` already assert the real Stage-B contract (the
    weakening from c60911eecd7/76ee70da4a7 was reversed ahead of this task — the strict assertions,
    the drain-to-`PENDING = 0` loop, and the exact-zero `fsck_unreachable` check are already back in
    place) and are registered as known-red in `tests/broken_tests.yaml` (entries
    `05008_ca_gc_snap_prune`, `04290_content_addressed_no_leftovers`,
    `04295_content_addressed_mutation_no_leftovers`) purely because production destruction is still
    suppressed. For these three: remove all three `broken_tests.yaml` entries and confirm the tests
    pass with NO change to the test files themselves — a pass here is the flip doing real work, not an
    assertion being loosened to fit it.
  - `CasGcLog.EmitsStartFinishWithCounts` (`gtest_cas_gc_log.cpp`) and the displacement test in
    `gtest_ca_wiring.cpp` (currently `EXPECT_GT(after.unreachable, 0u)`) are STILL weakened in the
    source, per their own markers. For these two: restore each to its real assertion (per its marker;
    the wiring test to `EXPECT_EQ(after.unreachable, 0u)`) and prove it green with the assertion
    restored.

  **`04290` and `04295` passing with their drain-to-`PENDING = 0` loop intact is the end-to-end proof of
  this task** — they are the only tests that watch the whole pipeline reclaim, so if they cannot drain,
  the flip did not deliver destruction no matter what the delete-family metrics say. Paste each run's
  output. If any of the five cannot be closed out, that is a FINDING about the flip, not a reason to
  leave the marker (or, for the first three, the `broken_tests.yaml` entry) in place.

  Exit condition: `grep -n "STAGE-A RETURN ITEM"` returns NOTHING, and none of the three
  `broken_tests.yaml` entries named above remain.

- [ ] **Step 5: fix the `PENDING` gauge while you are in those two files** (BACKLOG
  `{#stateless-pending-double-count}`): `PENDING = pending_candidates + pending_condemned +
  pending_retired` double-counts, because `pending_condemned` is already `candidates + retired` per its
  own doc in `Gc/CasGc.h`. Harmless while the loop cannot reach zero under suppression, but Step 4's
  flip makes the drain-to-zero comparison live and every figure it prints is twice the truth. Use
  `pending_condemned` alone.

- [ ] **Step 6: Commit** the `broken_tests.yaml` removals and the two gtest restorations as their own
  commit, so the flip and the test closeout are separately revertable. Then confirm `grep -n "STAGE-A
  RETURN ITEM"` returns NOTHING — a surviving marker after this task means either a known-red
  registration or a weakened assertion was left behind.

### Task 8: R2+R3 — writer duty queue + orphan nomination, one coherent change {#task-8}

**Files:**
- Modify: `.../Pool/CasPartWriteTxn.cpp` (`~PartWriteTxn` unconditional retirement `:119`;
  staged-body cleanup `:1438`), `.../Gc/CasOrphanManifestSweep.cpp` (the nomination path),
  `.../Gc/CasBlobInDegree.cpp` (the NEUTRAL nomination input — bypassing B2 ordinals and
  unmatched-remove accounting, `~:591` constraint), `.../Gc/CasGc.cpp` (adopting nominations in
  the round's `gc/state` CAS)
- Modify: `docs/superpowers/models/CaRefWriterCleanupCore.tla` + configs (+ its runner) — R2's
  duty queue and uncertain-grant guard; AND `docs/superpowers/models/CaRefFoldClampRecoveryCore.tla`
  (or the fold model the audit shows owns the ordering — named, not conditional) — R3's
  nomination REQUIRES its own model gate [codex finding 12]: a named sabotage/control pair, at
  minimum `_sab_deletebeforeadoption` (exact-token delete issued before the nomination's
  `gc/state` CAS adoption → RED on the ownership invariant) and
  `_sab_nominationcontaminates` (nomination fed through B2 ordinals / unmatched-remove
  accounting instead of the neutral input → RED on the accounting invariant), plus the green
  control. Sabotage-first, name-asserted runners, parenthesised ghosts (the phase conventions);
  model commits land BEFORE the C++ commits.
- Create: `src/Disks/tests/gtest_cas_writer_duties_nomination.cpp`

**Interfaces (register R2/R3 verbatim):**
- R2: an in-memory duty queue retried while the mount lives + the successor-seal path for crash
  remnants; "do not retire a build while an owner-grant outcome is uncertain" — the every-attempt
  wedge (Stage A Task 4) is the primitive the retirement check consults.
- R3: the sweep exact-GETs and DECODES the manifest FIRST; feeds its `BlobRef`s through a
  NEUTRAL nomination input; nominations adopted in the round's `gc/state` CAS; ONLY THEN
  exact-token-delete. Death-after-adoption leaves a manifest leak that is safe to retry when
  rediscovered (NOT guaranteed-retry — the honest r8-6 wording). Manifest keys are immutable
  monotone identities; a different token at the same key is illegal ABA → retain + surface.
- The S42 stale-edge defect (sweep strands folded `+1` edges) must have a direct regression
  test reproducing the recorded S42 shape
  (`reports/2026-07-26-s42-stale-edge-repro/`).

- [ ] **Step 1: TLA first** (phase conventions): extend `CaRefWriterCleanupCore` with the duty
  queue + uncertain-grant retirement guard; new sabotage `_sab_retireuncertain` (retire while
  wedged-Unresolved) must go RED on the ownership invariant; runner updated, sabotages first,
  exact-name assertion. Commit the model change separately BEFORE the C++.
- [ ] **Step 2: Failing C++ tests**: build retired while grant wedged → refused (queue holds
  it); duty queue drains on wedge resolution both ways (adopt/reject); crash remnants cleaned
  via the successor-seal path; sweep nominates a swept manifest's blobs (in-degree rows appear,
  B2 ordinals and unmatched-remove UNCHANGED — assert both accountings byte-stable); nomination
  adopted in `gc/state` before any delete (op order); ABA token at manifest key → retain +
  surface; the S42 regression.
- [ ] **Step 3:** → FAIL, implement, PASS. **Step 4:** CA gate + lanes + the extended model's
  runner green. **Step 5: Commit**
  `ca: writer/gc — duty queue, uncertain-grant retirement guard, neutral orphan nomination (R2+R3)`.

### Task 9: R1 closure note — verbatim-file rebirth aliasing {#task-9}

**RE-SCOPED 2026-07-29 by the amendment, from "write the R1 design spec" to "record where R1
went".** The reason, stated for the reviewer: R1's direction offered two alternatives — qualify the
file layer by incarnation, or add a read-side life gate. The directive PICKS the first one and has
it IMPLEMENTED inside Stage B (Tasks 4b/5/6). A 2-3-page spec with an alternatives table and
falsification conditions for a decision already taken and executed would be retrospective
paperwork, and the house rule against documenting transient state as durable applies. What still
has value is the audit trail — R1 was registered as a NAMED hazard, so it must be closed with
evidence, per sub-hazard, and any residue the re-key does NOT cover must be named rather than
quietly dropped. Hence: a short closure note, plus ONE genuine open question (loose mountpoint
objects, which the directive names as explicitly unchanged). Scheduling: after Tasks 4b and 6, so
the note records what landed and not what was intended.

**Files:**
- Create: `docs/superpowers/cas/2026-07-XX-r1-verbatim-file-aliasing-closure.md` (date at
  execution; a `cas/` note, not a `specs/` design — it records outcomes)
- Modify: `docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md` — R1's entry gets its
  disposition; `docs/superpowers/cas/BACKLOG.md` if any residue survives

- [ ] **Step 1:** Per R1 sub-hazard, record WHERE it went, with the commit and the test that
  proves it — no prose-only claims:
  (a) unqualified file keys aliasing a reborn namespace → closed first by Task 4b's re-key and finally
  by Task 4d's opaque-id state prefix (name both tests);
  (b) the `namespaceFilesReadable` TOCTOU (`ContentAddressedMetadataStorage.cpp:1231`) → record Task
  6's final verdict: the rebirth arm of its premise is dead (a life-keyed read cannot resolve into
  another life), fresh name resolution rejects `Removing`/absent at the catalog cut,
  and an already-held handle deliberately keeps stale-or-`NotFound`; therefore the gate and
  `namespaceIsRemoved` were deleted without adding a hot-path catalog GET;
  (c) the LIST-derived physical-empty proof (`Gc/CasGc.cpp:2998-3010`
  `namespacePhysicallyEmpty`) → its `_files` arm deleted by Task 4b (rebirth-does-not-wait), `_files`
  out of every lifecycle-gating pass; Task 5 deletes the lifecycle-specific cleanup attempt. The
  perpetual dead-life janitor is the sole owner of namespace-life debris, while the orphan sweep alone
  owns orphan-manifest bytes; LIST omission remains leak-only;
  (d) migration → Task 4d's generation-6 layout cut followed by Task 5's generation-7 wire cut,
  both recreate-only, Constraint 14.
- [ ] **Step 2 (the one open question):** Determine whether LOOSE MOUNTPOINT OBJECTS carry a
  rebirth-aliasing hazard of their own. They are outside namespace ownership and the directive
  keeps them unqualified, so the answer is expected to be no — but ANSWER it from the code, do not
  assume: if a mountpoint object's identity can be re-resolved by a same-name reborn namespace,
  that is a surviving hazard, it is OUT of this amendment's scope, and it gets a new register item
  + a `BACKLOG.md` entry naming the exposure (and only then a small spec, as its own unit of work).
- [ ] **Step 3:** Commit `ca: docs — R1 verbatim-file aliasing closed by the namespace-life re-key`.

### Task 10: TLA debt from the phase — seven review units {#task-10}

[Codex finding 17: the original single-unit framing could hide an evidence-sensitive model
retirement inside a mechanically enormous diff. Each sub-task below is dispatched, reviewed and
committed INDEPENDENTLY, with its own before/after results artifact.]

**Task 10a — `listedTok` semantic audit (after Task 5b).** Audit `CaGcRootLocalPartManifestCore`'s `listedTok`
(`:79`) + the skip gate (`:866`) against v9: does the model's "discovery observes from LIST"
premise survive universe-from-catalog (Stage B Task 4) AND LIST-independent recovery (Task 5b —
after the amendment, LIST may only offer a newer candidate, diagnostics or garbage nominations, so
a model in which LIST determines committed history is modelling a premise the code no longer has)? Verdict into the model's RESULTS file;
if the premise died, affected configs get rewritten or retired WITH the retirement recorded
(the phase's unaudited-residual section gets its answer). Commit alone.

**Task 10b — driver expectations, by model family.** Author expectations for the 123 configs
behind the nine single-config drivers; name-asserting runners per the `run_mount.sh` convention
(sabotages first, exact-name, `temporal` kind where properties are liveness). One commit per
model family, each with its runner output pasted.

**Task 10c — runnerless models.** `CaGcLeaseCore`, `CaGcRoundDeferCore`,
`CaGcShardIncarnationCore`, `CaB140DangleMerge` (4 `m_*.cfg` configs): each gets a runner or a
RECORDED retirement decision. Commit alone.

**Task 10g — the TLC jar itself is broken for temporal properties, and three expectations may be passing
for the wrong reason.** Established 2026-07-30 with a stated-in-advance prediction that held: the supplied
`tmp/tla2tools.jar` (TLC 2.19, 2024-08-08, rev `5a47802`) reports a temporal violation for a module whose
only property is **`EventuallyTrue == <> TRUE`** — a tautology — with an initial-state-plus-stuttering
counterexample. Removing the fairness conjuncts changes nothing, so fairness is not the mechanism. A current
official jar (`tla2tools` 2026.07.18.145032, rev `30cc360`) accepts both that module and the
`CaRefWriterCleanupCore_empty_builds.cfg` configuration that started the investigation.

**The blast radius is bounded, and the bound is itself evidence.** 43 configs carry a `PROPERTY`/`PROPERTIES`
line, and the green-expecting ones PASS — which they could not if the false violation were universal. So the
defect needs a reachable stuttering-only suffix, i.e. a state with no enabled action, which is exactly what an
empty entity set produces. What is genuinely at risk is the other direction:

- [ ] **Re-validate the three `temporal` expectation rows under a jar that passes the `<> TRUE` smoke test**
  (in `run_refwcleanup.sh`, `run_buildrootprecommit.sh`, `run_disklifecycle.sh`, `run_gcrounddefer.sh`,
  `run_foldclamp.sh`). **A row that EXPECTS a violation passes under a checker that violates everything** —
  so each of those three is a candidate for having been green for the wrong reason since it was written.
- [ ] **Adopt the smoke test as a gate on the checker, not just as a note.** Before trusting any temporal
  result, run `<> TRUE`; if it violates, the checker is lying and no temporal verdict from it means anything.
  Two lines, and it would have replaced this entire investigation.
- [ ] **Do NOT add `ASSUME Builds # {}` to hide the empty-set result** — that converts a checker defect into
  a model restriction, and the empty set is semantically valid for that model and that property.
  `CaBuildRootPrecommit`'s `ASSUME` is right only where the model genuinely requires a non-empty domain.
- [ ] Decide whether to upgrade the pinned jar or keep the old one for safety checking and the new one for
  temporal work; either way the choice must be recorded where the runners can see it.

**Task 10f — the empty-set blind spot, and the unmodelled destructive gate (after Task 5; before
Task 7b).** Two findings from 2026-07-30, both verified by inspection; this sub-task is worth more
than the rest of Task 10. Its destructive-gate model and every applicable empty-universe configuration
are a hard predecessor of Task 7b: `Task 5 → Task 10f → Task 7b`.

1. **The destructive gate is not modelled at all.** `Gc::fold` decides destruction on
   `frontier_complete = universe_authoritative && frontier_proven == frontier_namespaces`. No model
   encodes that comparison. The word "universe" appears once in `CaGcRootLocalPartManifestCore.tla`, in a
   comment about the fence universe for `coordFence`; "proven" appears only in comments about a watermark
   proving a build dead. `CaRefCatalogCore.tla` — the model of the object that becomes the universe — does
   not mention the gate. Model it: the gate that authorises irreversible deletion is the last thing in this
   design that should rest on code review alone.
2. **And no config in this tree can reach an empty entity set, so no model can catch a vacuous-empty
   defect.** Verified across every `.cfg`: zero of them make `Namespaces`, `Blobs`, `Builds`, `Leaders`,
   `Shards`, `Tables`, `Roots`, `ManifestInstances` or `Entries` empty. The only empty sets are auxiliary
   (`TreeHashes`, `UniqueToBuildTree`). Since `CONSTANTS` are fixed per config, the empty case is
   unreachable BY CONSTRUCTION — an under-approximation, which is the defect direction, not the safe one.
   Register finding R11 (`{#r11-empty-universe-vacuous}`) is that blind spot's first known member, and it
   was found by reading code, not by the models that exist to find it.

   Add an empty-set config to each model where an empty entity set is reachable in production, and where
   it is NOT reachable, say why in the model rather than leaving the absence to look like coverage. Expect
   this to turn up more than one red; each is a finding, not a config to adjust.

**Task 10e — the lane battery's two witness debts** (carried from BACKLOG
`{#lane-witness-names-more-than-it-proves}`, which had no executing task until now). Two fixes in
`docs/superpowers/models/`: (1) `CaRefLaneCore_RESULTS.md` calls `saw_retry_created` a
"retry-created adoption" witness, but the flag is set in `ObserveDurable`, so it witnesses that the
retry reached DURABILITY — not that the adoption install happened. Correct the wording; prefer
deleting the over-claiming phrase over rewriting it. (2) No witness asserts the `Wedged → Ready`
durable-adoption arm at all — add one that fires on the adoption install itself, and re-run
`run_reflane.sh` (currently 15/15) pasting the tail. Neither touches the blocker-dissolved verdict,
whose proof runs through `ReadyCaughtUp`. Commit alone.

**Task 10d — phase-runner classifier.** Fix the five phase runners' inert `[A-Za-z_]+` →
`[A-Za-z0-9_]+`; re-run all five end-to-end once (expect identical results; paste tails).
Commit alone; final commit updates `models/README.md` + `cas/06-tla-models.md`.

### Task 11: Stage B gates {#task-11}

- [ ] **Gate step: run clang-tidy over Stage B's own sources — startable EARLY, in any idle window.** The
  profile exists: `BuildTypes.AMD_TIDY` (`ci/defs/defs.py:354`), whose cmake line in
  `ci/jobs/build_clickhouse.py` is a **Debug, no-sanitizer, x86_64-toolchain** build with
  `-DENABLE_CLANG_TIDY=1`. **Use the AMD profile, not `arm_tidy`** — CI runs tidy only in the ARM build
  (`build_clickhouse.py`: "Run it only in the arm_tidy build to avoid adding overhead to every build"), and
  cross-compiling for ARM locally buys nothing here and costs a toolchain. The repo's `.clang-tidy` (10 KB)
  supplies the check set, so nothing needs configuring beyond the switch.

  Practical shape, since it is a long build and must not disturb anything in flight:
  - Configure it in **its own build directory** (`build_tidy`), never by adding the switch to an existing
    one — turning `ENABLE_CLANG_TIDY` on in `build/` or `build_asan/` makes every later incremental build
    pay for tidy and invalidates their caches.
  - Redirect the build to a log inside that directory; no `-j`, no `nproc`.
  - **Scope the reading, not the build**: the interesting output is diagnostics under
    `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` and `src/Disks/tests/gtest_ca*`,
    `gtest_cas*`. Everything else is pre-existing and not this stage's business — report it, do not fix it.
  - `clang-tidy-cache` is what makes this affordable on a second run; CI wires `CTCACHE_DIR`, so a local run
    that sets it too will not pay full price twice.

  **Why it is a gate step and not a nice-to-have:** this stage added ~7500 lines of C++ across 109 files,
  including a lifecycle, a new format codec and several new lock-holding paths — and two of the defects found
  this stage (a `chassert` over a handled branch, a `std::unique_lock` unlocked on a throw path) are exactly
  the shapes a static analyser is good at. Start it during any wait; read it before the stage closes.

- [ ] **Gate step: sweep Stage B's own comments for rules that only a reader can enforce.** Four rules failed
  this stage while living in prose ("every term of `clean()` appears here", the gate suite list, the `_ckpt`
  cleanup's ownership premise, the `per_ns_shard` pruning claim); the two that were converted to executing
  checks — `clean()` computed from `kFsckHardFindings` with a `static_assert` in three TUs, and the suite-list
  generator failing on any unclaimed suite — both held immediately. So grep this stage's diff for
  "whenever/always/must also/in the same change" and, for each, either convert it to something that fails a
  build or a test, or record in `BACKLOG.md` why it cannot be. A rule that only a reader can enforce is one
  context loss from not holding, and this stage measured that four times.

**Files:**
- Create: `docs/superpowers/cas/2026-07-XX-stage-b-RESULTS.md`
- **RESIDUAL-CLEANUP GATE ROW, placed by the placement sweep (2026-07-30).** Several review findings
  were deferred to a "final-review triage" that this plan does not contain, so they had no executor:
  Task 1's review minors 1-7 (its minor 8 — the bump-B verification — moved to Task 4 instead,
  because a deferred VERIFICATION is simply not performed), and the Task-1 re-review's MINOR-B plus
  NITs C-F. Their home is here, as an explicit gate row rather than a hope: before the verdict, walk
  that list, fix what is still true, and record each as fixed / no-longer-applicable / accepted with
  one line of why. A cosmetic finding with no executor is what the sweep was run to find; this row is
  where the cosmetic ones die. NOTE the one exception already re-homed: MINOR-B (`rebuildBaseline`'s
  gen-0 nested-shape exposure) goes to Task 1c beside IMPORTANT-A, because it is the SAME CLASS at a
  different site and splitting one audit across two tasks is how the first half got missed.
- **PRESERVE (not create): the destructive soak's ARTIFACTS.** Task 12 does performance research on
  this soak, and the predecessor report
  (`docs/superpowers/reports/2026-07-29-gc-perf-audit-soak.md {#specimen-lost}`) exists partly to
  record that a specimen was destroyed before it could be sampled. So Step 3c's run is a SPECIMEN:
  keep the server logs, the `content_addressed_log` / `content_addressed_garbage_collection_log`
  tables (or their dumps), the profile artifacts and the harness output under a named directory, and
  write that path into the results file. Do NOT tear the environment down at the end of Step 3c —
  Task 12 samples it.

- [ ] **Step 1:** Full CA gtest gate vs Task 0 baseline.
- [ ] **Step 2:** All CA integration lanes local (Stage A Task 14's list) green.
- [ ] **Step 3:** Four REQUIRED soak runs are green: (a)–(c) below and the separate 90-minute
  general soak (d). The fourth run is not folded into any of (a)–(c):
  (a) churn soak — create/drop namespaces at ≥1/s for ≥30 min under load: catalog entry count
  returns to baseline (O(Creating+Live+Removing) — the r9-6 flatness claim), zero alias reads,
  fsck clean; (b) rebirth adversarial scenario — drop/recreate under concurrent readers +
  stale cleanup resume (Task 5's test at soak scale), and after the amendment the readers must
  include NAMESPACE-FILE readers/writers, not only ref readers: zero reads resolving to a newer
  incarnation across the whole run, `_files` debris from dead incarnations trending to zero via the
  janitor without ever blocking a rebirth; (c) decommission scenario — victim with hidden `Removing`
  entries recovered under the claimed fence, completed rows deleted only by GC and leftover opaque
  checkpoints reclaimed by the janitor (Task 7 at soak scale); (d) a phase-3 `--duration 90m` general
  soak, same PASS criteria as Stage A, is the fourth required run and is separate from (a)–(c).
- [ ] **Step 3c (2026-07-30 GC directive): THE SEQUENTIAL-BASELINE DESTRUCTIVE SOAK.** Run this
  required destructive workload during the fourth, 90-minute general soak, not as an uncounted fifth
  specimen and not as a variant of (a)–(c): run the destructive round on **the current
  sequential implementation** — no `MultiDelete`, no parallel deletes, no delete-side concurrency
  (Constraint 18). Its purpose is an honest cost baseline; "faster" is explicitly not a goal here.
  **Cost inventory — every line MEASURED, and a line that cannot be measured is named as un-timed
  rather than estimated:** `pending_deletes`; owner-removed manifest deletion; orphan-manifest sweep;
  ref-object cleanup; namespace cleanup; generation pruning; and time plus number of ROUNDS to
  fixpoint. Report per line: invocation count, S3 operation counts by verb, wall time, and share of
  round time.
  **No new instrumentation is needed for six of the seven, and one has no row of its own — know which
  before you start.** `GcPhaseTimer` is always on ("no setting, deliberately") and already emits a
  duration + phase-metrics row per phase into
  `system.content_addressed_garbage_collection_log`, so five inventory lines read straight off
  existing phase rows: `pending_deletes` (metrics `redeleted`/`graduated`/`deleted`/`absent`/
  `replaced`/`spared`), `manifest_deletes` (`attempted`/`deleted`), `orphan_sweep` (its
  `ManifestSweepResult` retention breakdown), `ref_object_cleanup` (`Gc::cleanupRefObjects`), and
  `namespace_cleanup` (`attempted`/`deleted`/`leaked`/`suppressed` plus
  `janitor_pages`/`janitor_keys`/`janitor_deleted`, as reshaped by Task 5).
  **Generation pruning is the
  exception: `Gc::pruneSupersededGenerations` runs INSIDE the `round_commit` phase with no row of its
  own**, so its cost is currently entangled with commit cost — measure it by its own metrics
  (`generations_visited`/`pruned_through`/`generations_referenced`) plus the shared
  `deletePrefixWholesale` primitive it uses, and if that still cannot separate it, it goes into Task
  12's un-timed-spans list by name rather than being estimated. Rounds-to-fixpoint comes from the
  round sequence itself.
- [ ] **Step 3d: the six result criteria, as gate rows.** Each is PASS/FAIL on evidence; a row
  without a measurement is a FAIL, not a blank:

  | # | Criterion (directive §Критерии результата) | Measured by | PASS |
  |---|---|---|---|
  | 1 | Healthy rounds really do perform destructive work | per-family delete counts per round in `system.content_addressed_garbage_collection_log` | every family that has work nonzero on healthy rounds; no family silently inert |
  | 2 | `ca-fsck --detail` finds no dangling / stale-edge | `ca-fsck --detail` at soak end AND at a mid-soak checkpoint | zero dangling, zero stale-edge, both runs |
  | 3 | Backlog reaches zero STABLY | `pending_deletes` + cleanup backlog sampled per round to fixpoint | reaches zero and STAYS zero across ≥3 further rounds (a single zero sample is not stability) |
  | 4 | Holds/anomalies still suppress every irreversible path | inject one hold and one anomaly during the soak | all delete families inert for those rounds, per family, and the round still completes |
  | 5 | After probe-A removal there is no second full stream LIST | LIST counts per round attributed by prefix and phase | exactly ONE full `cas/ns/stream/` enumeration per round, on EVERY round including those probe A used to sample; the bounded `cas/ns/` janitor page is reported separately and is never mistaken for a second hot scan |
  | 6 | Phase timings + S3 operation counts give the baseline | the Step-3c inventory | recorded as the explicit `MultiDelete`/concurrency baseline, with the un-timed spans named |

- [ ] **Step 3e (namespace-life amendment, insert-path guard):** the dedup-log-bearing workload's namespace-file
  operation profile is UNCHANGED versus the Task-4b baseline (Constraint 16) — compare the
  per-operation backend request counts from the soak's `content_addressed_log`/ProfileEvents, not
  a micro-benchmark; any increase on that path is a Stage-B FAIL, not a note.
- [ ] **Step 4:** Write the results file: battery table + `STAGE B: PASS`/`FAIL` verdict line +
  the post-B residual list (what remains open: R4 registry, head-CAS north star §10, the
  `ApplyPending` debug-only evaluation from `{#follow-ups}`, and whatever Task 9 Step 2 concluded
  about loose mountpoint objects — R1 *implementation* is no longer on that list, the amendment
  landed it). Include the Step-3c/3d destructive-baseline table and the specimen path; the verdict
  line stays `STAGE B: PASS`/`FAIL`.
- [ ] **Step 5: Commit** `ca: stage B — gate battery results + verdict`.

### Task 12: GC performance research on the destructive baseline {#task-12}

[The 2026-07-30 GC directive's DELIVERABLE: "Провести исследование производительности GC на этом
soak-е и написать новый документ вида `docs/superpowers/reports/2026-07-29-gc-perf-audit-soak.md`" —
its successor for the destructive baseline. Research and write-up ONLY: this task implements no
optimization (Constraint 18 — `MultiDelete` and concurrency are the next round's work, and this
document is what justifies them).]

**Files:**
- Create: `docs/superpowers/reports/2026-07-XX-gc-destructive-baseline-perf.md` (date at execution) —
  the successor report; it must link the predecessor
  `docs/superpowers/reports/2026-07-29-gc-perf-audit-soak.md` and say which of its ranked
  opportunities the destructive baseline confirms, refutes or leaves untouched
- Modify: `docs/superpowers/cas/BACKLOG.md` — each ranked opportunity becomes a backlog entry, so the
  report is actionable rather than admired

**Interfaces — the report's required content, in the house shape the predecessor establishes:**
- **Scope + the evidence rule** up front: which specimen, which artifacts, what the numbers can and
  cannot answer (predecessor `{#scope}`).
- **Phase decomposition** of the destructive round: per-phase wall time and S3 operations by verb,
  covering every line of Task 11 Step 3c's inventory — `pending_deletes`, owner-removed manifest
  deletion, orphan-manifest sweep, ref-object cleanup, namespace cleanup, generation pruning — plus
  time and ROUNDS to fixpoint.
- **Un-timed spans NAMED, never estimated silently** — the predecessor's
  `{#fold-cost-structure}` "what is measurable and what is not" discipline. A span with no
  instrumentation is listed as un-timed with the reason, and if it is load-bearing the report says
  what instrumentation would answer it. An estimate presented as a measurement is the one failure
  mode this section exists to prevent.
- **Before / after where it is honest, and an explicit refusal where it is not.** Probe-A removal HAS
  a before/after (LIST count and round time, pre- and post-Task-7a) and gets one. Destruction does
  NOT: it never ran in production before Task 7b, so there is no "before" — say that instead of
  manufacturing a comparison against a suppressed round, which measured a different workload.
- **Ranked opportunities for the NEXT round**, each with the measurement that motivates it and a
  falsification condition — specifically whether `MultiDelete` and delete concurrency are worth it,
  which phase they would touch, and what the baseline says the ceiling is.
- **Evidence index**: every figure traceable to an artifact path + query/command.

- [ ] **Step 1:** Sample the preserved specimen (Task 11's artifact directory). Do NOT re-run the
  soak to get numbers — a second run is a different specimen, and mixing the two is how the
  predecessor's `{#cautions}` section came to exist.
- [ ] **Step 2:** Write the report to the shape above.
- [ ] **Step 3: re-read every figure from the artifacts AT WRITE TIME**, immediately before
  committing — recalled numbers go stale as the analysis progresses, and a figure that was correct
  when measured can be wrong by the time the sentence around it is finished. Any figure that cannot
  be re-derived from the evidence index is removed, not softened.
- [ ] **Step 4: Commit** `ca: reports — GC destructive-baseline performance research`.

---

## Follow-ups — recorded, NOT implemented here {#follow-ups}

- **Evaluate whether `ApplyPending` can later become debug-only** (directive, verbatim: "Record a
  follow-up to evaluate whether `ApplyPending` can later become debug-only, but do not make that
  change here"). No task in this plan may act on it; `RefApplyState` refactoring is out of scope
  (Constraint 17). Task 11 Step 4 carries it into the post-B residual list.

## Self-review checklist {#self-review}

1. Spec coverage: INV-3 → T2/T4/T4b/T5; §2 read contract + r9-3 → T1/T1c/T6; §3 creation → T3,
   removal → T5, recovery-ownership generation → carried from Stage A T6 (unchanged here);
   catalog capacity both halves → T2 (entry/byte) reusing Stage A T8 arithmetic; §5
   catalog-driven universe → T4; INV-4 `_ckpt` → T4 (re-key) + T4c (strengthening); §4 recovery
   → T5b; register: R2/R3 → T8, R5 → T7, R1 → T9 (RE-SCOPED to a closure note — the `_files`
   half is closed in code by T4b/T6), R6 accepted (no task — by design), R7 done in Stage A T12,
   R8 done during the phase.
2. Ledger obligations mapped: token-exact reconciliation at call site → T3; creator install
   generation+token → T3; the generation-only three-site trio (slot-occupy/`_ckpt`/install) is
   COMPLETED IN STAGE A Task 6 with its deterministic bump tests — Stage B Task 3 adds the
   SEPARATE two-credential catalog site, not a trio member [codex r2 finding 7]; deposited
   incarnation + capture-time correctness → T5 (refs AND `_files` debris after the amendment);
   `listedTok` audit + drivers + runnerless + classifier → T10a-d; destructive-gate/empty-universe
   model → T10f → T7b; destruction enablement ordering → T7b.
3. Every task name-checks its TLA counterpart where one exists; model edits follow phase
   conventions and are grouped in T8 (register models) and T10 (debt) — the five PHASE models
   stay sealed except T10's classifier-only runner fix.
4. **Directive coverage (2026-07-29 amendment), section by section:** §1 generalize identity →
   T1c; §2 incarnation-key namespace files → T4b (keys, rebirth, request profile) + T5 (cleanup
   half) + T6 (read/write half); §3 LIST-independent recovery → T5b; §4 strengthen `_ckpt` →
   Constraint 15 + T4c (behavior change explicitly sequenced AFTER T4's re-key); impl-1
   `prepareRefChunk` → T1b; impl-2 `chooseRecoveryGrounding` + LIST audit → T5b; impl-3 snapshot
   publication → T6b; namespace-file implementation requirements → split T4b/T5/T6 as listed in
   the overview mapping; dedup performance constraint → Constraint 16 (asserted in T4b Step 1);
   out of scope + `Poisoned` clarification → Constraint 17; required tests → T1c (compile-time
   absence, legacy `_files` refusal), T4b (LIST-hidden old file across rebirth; request-count
   parity incl. dedup-log rotation), T5 (stale cleanup resumed after a new incarnation),
   T5b (recovery equivalence under full/empty/partial/reordered LIST; missing `_ckpt` for
   `Live`), T4c (conflicting `_ckpt.life_epoch`), T6 (stale reader; delayed writer finalized
   — **the delayed-writer test is REQUIRED, not representative**: the finalize lambda captures
   `this`, so a future edit can re-derive a life AT FINALIZE TIME and still compile, and two
   `NamespaceLifeId` values are indistinguishable to the type system, so no compile-time fence can
   catch it. That test is the only guard between such an edit and a silent regression. A wrapper type
   for "life captured at admission" WOULD make it type-checkable and is deliberately rejected as
   over-engineering for one call site — which is exactly why the test may not be dropped or weakened
   after rebirth; zero catalog GETs on namespace-file hot paths); `ApplyPending` follow-up →
   `{#follow-ups}`, implemented by nobody.
5. **GC directive coverage (2026-07-30), element by element:** §Sequence-1 delete probe A → **T7a**
   (+ R7's supersession note in the register and the spec §5 correction, both in T7a's commit);
   §Sequence-2 destructive enablement with the gate formula VERBATIM → **T7b** (Interfaces block +
   Step 1b's per-term suppression asserts; the formula is unchanged, only what `frontier_complete` may
   become changes); §Sequence-3 sequential-baseline soak + the seven-line cost inventory → **T11 Step
   3c** (+ the specimen-preservation obligation in T11's Files, because the predecessor report exists
   partly to record a specimen destroyed before sampling); §Критерии результата, all six → **T11 Step
   3d** as explicit PASS/FAIL gate rows; §Deliverable → **T12** (successor report, research only);
   "no `MultiDelete`, no parallel deletes" + the two-separate-changes rationale → **Constraint 18**,
   binding on T7a/T7b/T11/T12. The R7 register entry is the only cross-reference the directive named,
   and it landed with the plan amendment rather than being deferred to execution.
6. Amendment interactions a reviewer must check on sight: the `stageATransition()` retirement gate
   (T6 Step 1b) now covers the FILE call sites T1c/T4b introduce; `gtest_cas_list_liar_end_to_end.cpp`
   has TWO legitimate Stage-B editors after the amendment (T5b for the capstone sentinels it
   deliberately reds, T7b for the kill-shot) — Task 4's "only edit" line is amended in place;
   Task 4d makes physical identity opaque and splits immutable `cas/ns/stream/<life_id>/` from
   point/path-addressed `cas/ns/state/<life_id>/`; the fold's one hot stream enumeration and Task 5's
   one cursor-paced ownership-tree janitor are intentionally different consumers. A review that
   collapses them either reintroduces `_files` into every fold LIST or makes LIST omission a permanent
   leak, so both prefix and phase assertions are load-bearing.

### Task 13 (post-Stage-B): split the two files that are 18% of the subsystem {#task-13}

**Deliberately AFTER Task 12, and the reasons are not stylistic.** `Gc/CasGc.cpp` (4679 lines) and
`Pool/CasRefLedger.cpp` (4249) are 18% of the ~50k-line CAS subsystem between them. Size here has a measured
cost: this stage's `chassert`-over-a-handled-branch sat four lines from the branch it killed, the double-unlock
that masked a real error class lived in the same file, and both survived several reviews.

Why after 12 rather than sooner:
- **Not while a Critical is open in that code.** A refactor landing beside an unfixed data-loss defect makes the
  defect harder to see and its fix harder to review.
- **Not between Task 11's gates and Task 12's measurements**, which would invalidate the performance baseline
  the report is built on.
- Both files were still changing throughout Stage B, so any earlier split would have been re-split.

Task 5 Checkpoint 7.5 deliberately creates `CatalogLifecycleReconciler`, immutable `RoundInput`/`RefPlan`,
the immutable-life runtime boundary and a separate `GcMaintenanceState` before this task. Those are semantic
ownership changes required for correctness and for the janitor's safe implementation, not this task's
mechanical line-count split. Task 13 must preserve those interfaces and move the remaining lane/install/fold
regions around them without redesigning their behaviour or invalidating the Task-11/12 baseline.

- [ ] **Write the equivalence goldens BEFORE moving anything.** This campaign's standing rule: a fence added
  after an extraction tests the new shape rather than the preserved behaviour. Capture request-count and
  request-order goldens for the paths being moved, prove them stable on the unmoved tree, and only then move.
- [ ] Split along the seams the defects already revealed rather than by line count — the lane/append machinery,
  the install regions, and the fold's phase pipeline are three separate concerns sharing one translation unit.
- [ ] After the move, re-run the goldens unchanged. A golden that had to be edited to pass is evidence the
  extraction changed behaviour, not evidence the extraction succeeded.

### Task 14 (before upstreaming): strip every branch-local reference from the code {#task-14}

Nothing in this plan owned this, and it must happen before any of this work goes upstream: `docs/superpowers/**`
— plans, BACKLOG, register, reports — is branch-local and gets deleted, so every comment citing it becomes a
dangling pointer. Measured baseline: roughly **850** such references.

- [ ] **FIRST, separate the operator-visible ones — they are a different and worse class, and they must
  not wait for upstreaming.** A comment citing a task number dies with the branch; a **string literal**
  citing one is shipped to an operator in a log line or an exception message, where the reader has no
  possible way to resolve it and no reason to suspect it is internal. A sweep of the CA sources found
  **250** branch-local references, of which **4 are inside string literals**: two `recordAnomaly`
  payloads in `Gc/CasGc.cpp` ("expected until Task 5's removal-evidence check lands"), one in
  `Pool/CasRefCatalog.cpp` ("Live/Removing name is Task 5's (removal's) business"), and a format-version
  message in `Formats/CasPoolMetaFormat.cpp` naming "Stage B". The three that name Task 5 describe a
  check Task 5 itself writes, so **Task 5 rewrites them as part of building it** rather than leaving them
  for this task; the format message says "Stage B" where it means a format generation and should name the
  format instead. Re-run the sweep here rather than trusting this count: the sweep is one command and the
  number is a measurement, not a fact about the tree.
- [ ] **Strip the provenance, keep the reason.** For each reference, the comment either already states the
  reason it cites — delete the citation — or it does not, in which case write the reason and then delete the
  citation. A comment reduced to "per review C3" with no reason is a comment with nothing to keep.
- [ ] **Enumerate the coordination markers separately, because they are legitimate NOW and must go LAST.**
  `STAGE-A RETURN ITEM`, "this task edits exactly this line" and their kin exist to couple branch work; they
  are deliberate and greppable. List them, confirm each one's owning task has run, then remove them. A marker
  outliving its task is worse than none.
- [ ] **Triage the long blocks** (83, 65, 63, 61, 57 lines and down): each becomes structure, shrinks to the
  reason alone, or goes. A header block is the one place length is sometimes right — it documents the interface
  for code intelligence — but an essay is still not a contract.
- [ ] **Re-measure and record: comment share, longest block, remaining references.** The policy is checkable;
  leave the numbers where the next reader can compare.
