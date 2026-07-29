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

Constraints 1-11 of the Stage A plan
(`2026-07-28-cas-ref-chain-stage-a-streams.md {#global-constraints}`) apply VERBATIM to every
task here. Additional Stage-B constraints:

12. **[AMENDED 2026-07-29]** The incarnation qualifies THE REF LAYER AND NAMESPACE FILES:
    `<ns>/<inc>/{_log,_snap,_cleanup,_ckpt}` (`_cleanup` added by Task-1 review Important-2: the
    cleanup marker builds through the same life-scoped prefix and is one of
    `parseRefObjectKey`'s three kinds; a marker surviving into the next life would fake a
    completed removal, the exact rebirth alias INV-3 closes) AND
    `<ns>/<inc>/_files/<relative-name>` (directive §2 — the original ref-layer-only boundary left
    the hole that an old file omitted by LIST survives namespace removal and becomes visible
    after the same logical namespace is created again). Explicitly UNCHANGED, both directions:
    manifests keep `(namespace, mount-epoch, build-sequence)` identity and their existing
    globally unique build identities; LOOSE MOUNTPOINT OBJECTS stay outside namespace ownership
    and unqualified (they are why the plain-object component survives — Constraint 17); and the
    current DIRECT-OBJECT implementation of namespace files stays as it is — this amendment
    changes object identity, never file persistence.
13. Catalog admission refuses loudly; removal is NEVER refused (spec INV-3).
14. Format bump B (Task 4) is Stage B's single recreate-only bump — one bump, all re-keying
    behind it, ref keys and `_files` keys alike (Task 4b rides Task 4's bump; it does NOT add a
    second one). Legacy unqualified ref AND `_files` keys are rejected as corruption, not
    migrated (directive: "Keep the existing format-bump-B recreate-only migration; reject legacy
    unqualified keys").
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
| 5 | Removal lifecycle: terminal record, janitor, deposited-incarnation cleanup | §3 + directive §2 | 3,4,4b |
| 5b | `chooseRecoveryGrounding` + LIST-independent recovery + the LIST audit | directive §3/impl-2 | 4c,5 |
| 6 | Read-side contract: handles, pre-delete revalidation, namespace-file read/write closure | §2 + directive | 4,4b |
| 6b | `trySnapshotPublishOnce` → `tryPublishSnapshotAndAdvanceCheckpointOnce` | directive impl-3 | 4c,6 |
| 7 | R5 decommission duties | register R5 | 4,5 |
| 7a | DELETE probe A — the second full ref LIST and everything that serves it | GC directive §1 | 4,5b |
| 7b | Destruction enablement: `UniversePolicy::kDefault` → authoritative | staging contract + GC directive §2 | 4,5,6,7,7a |
| 8 | R2+R3: writer duty queue + orphan-blob nomination (one coherent change) + model extensions | register R2/R3, §9 | 4 |
| 9 | R1 closure note (verbatim-file rebirth aliasing) — doc only, RE-SCOPED | register R1 | 4b,6 |
| 10 | TLA debt: `listedTok` audit, unasserted drivers, runnerless models, classifier | phase follow-ups | — |
| 11 | Stage B gates: battery + churn/rebirth/decommission soak + the sequential-baseline destructive soak + verdict | §9 + GC directive §3 | all |
| 12 | GC performance research on the destructive baseline + the successor report | GC directive deliverable | 11 |

Task 10 is independent of the code chain and may be scheduled at any point (still one implementer
at a time). Task 9 is doc-only but is no longer schedule-free: the re-key must exist before it can
record where each R1 sub-hazard went. Task 11's soak REQUIRES Task 7b (destruction enabled) — a
soak with destruction still suppressed does not exercise Stage B's claims. Task 12 requires Task 11's
soak ARTIFACTS, not merely its verdict.

**Recommended execution order** (a topological order of the column above; the directive's
§Execution commit list is honored in its own relative order):

`1b → 1c → 2 → 3 → 4 → 4b → 4c → 5 → 7 → 5b → 6 → 6b → 7a → 7b`, with Task 8 anywhere after Task 4,
Task 9 after Task 6, Task 10 anywhere, then Task 11 and finally Task 12.

**The GC tail (Tasks 7a → 7b → 11's destructive soak → 12) is a SEQUENCE, not a set**, and the
2026-07-30 directive's rationale is why: probe-A removal is a performance change, destruction
enablement is a correctness change, and running them together makes the soak unable to attribute
either. Constraint 18 keeps the implementation sequential so the soak produces a baseline the next
round's `MultiDelete`/concurrency work can be measured against.

Directive commit → task: (2) pure preparation → **1b**; (3) general namespace-life identity →
**1c**; (4) ref and file re-keying → **4** (refs, already planned) + **4b** (files); (5)
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
  classify what `namespaceAllLivesPrefix` returns
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
  declares no conversion operator, and `RootNamespace`'s own constructor is `explicit`
  (`Primitives/CasTypes.h:52`), so nothing interconverts in either direction today; only explicit
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
- Produces: spec §5's fold shape — per round ONE catalog `GET`; cursors keyed by catalog
  entries `(ns, incarnation)`; unhinted namespaces carry verbatim; the frontier proof (Stage A
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

### Task 5: Removal lifecycle — terminal record, janitor, deposited incarnation {#task-5}

**INTERFACE NOTE from Task 1 (2026-07-29, controller-ledgered; RECONCILED with the amendment):**
Task 1 deleted `refsNamespacePrefix(RootNamespace)` — the only all-lives LIST prefix — per
Constraint 4, and it STAYS deleted. The lazy janitor here needs a NEW, differently-named helper to
enumerate foreign-incarnation debris under a known namespace. Do NOT "restore" the old overload —
the deleted-overload concept checks in `gtest_cas_namespace_life_id.cpp` will (correctly) fail the
build if you do; add the new name with its own tests instead.

**The amendment settles its shape: ONE helper for BOTH object families, named
`namespaceAllLivesPrefix(const RootNamespace &)`** — not the earlier suggestion
`refsAllLivesPrefix`, and not a sibling per family. Reason: once Task 4b re-keys files, ref objects
and `_files` objects live under the SAME `<ns>/<inc>/` prefix, so one LIST at `<ns>/` discovers
every family's debris in one request; two helpers would mean two LISTs for one job and a second
chance to forget one. The janitor classifies what the LIST returns by the parsers (a life-less or
otherwise unparseable key is anomaly-and-continue, never a throw — the standing rule the Task-1 fix
commit `67dd2666e75` restored). **Why this is not a smuggled re-add of the deleted overload, stated
for the reviewer:** it returns a PREFIX FOR LIST ONLY and addresses no single life's object; it has
no `Key` suffix and no key-building sibling; and it is unusable for a read or a write because every
read/write path demands a `NamespaceLifeId`. Task 1c's concept-negative battery is written against
the KEY/prefix helpers that ADDRESS ONE LIFE, so it must not (and does not) forbid this one.

**Files:**
- Modify: `.../Pool/CasRefCatalog.cpp`, `.../Pool/CasRefLedger.cpp` (terminal append path),
  `.../Gc/CasGc.cpp` (`runNamespaceCleanupPasses` `:2145` — the cleanup item shape), the
  namespace-removal call path (`RefOpKind::RemoveNamespace` usages)
- Create: `src/Disks/tests/gtest_cas_ns_removal_lifecycle.cpp`

**Interfaces:**
- Consumes: Tasks 2-4b; Stage A holds (a `Removing` namespace can hold like any other).
- Produces (§3 verbatim, plus the amendment's cleanup half):
  - Removal = catalog `Live → Removing` CAS (the admission bound frees immediately;
    `Removing` forbids NEW positive ownership), then the terminal record appended ONLY by the
    owning mounted writer or a successor that claimed and fenced that server root; GC surfaces
    stuck removals (a durable counter + log per round observing a terminal-less `Removing`),
    NEVER appends.
  - After the terminal record FOLDS: ONE explicitly BOUNDED, suppression-aware best-effort
    cleanup attempt (spec INV-3's "best-effort cleanup runs" — codex finding 13: it sits
    BETWEEN the terminal fold and the deletions; its failures defer to the janitor as leak-only
    work), then `_ckpt` deleted by exact token while `Removing`, then the catalog entry deleted
    (entry LAST) — the ordering asserted via the backend op journal; the entry vanishes without
    a physical-empty proof (surviving old-incarnation objects are structurally inert — foreign
    prefix) **and after the amendment that explicitly covers `_files`: `_files` is REMOVED from
    mandatory namespace-removal LIST deletion (directive), and namespace rebirth must NOT wait for
    `_files` to become physically empty.**
  - **The `Removing`-without-`_ckpt` window is owned HERE, not by recovery** (directive §3):
    between the `_ckpt` delete and the entry delete the entry is `Removing` with no `_ckpt`, and
    Task 5b makes recovery REFUSE to ground exactly that shape. So the removal driver RESUMES it on
    the owning writer's next mount — idempotent, because the `_ckpt` is already gone and the resume
    is the exact-CAS entry removal alone. For a root that never returns it is Task 7's
    `_ckpt`-absent branch. Neither path may RE-CREATE `_ckpt` (that would resurrect a life the
    terminal record already closed).
  - Lazy janitor: whenever cleanup LISTING happens to return foreign-incarnation debris under a
    known namespace, delete it (omission = deferred cleanup, leak-only direction) — implemented
    inside `runNamespaceCleanupPasses`, gated by `suppress_destructive` like every destructive
    site. **After the amendment the janitor is the ONLY reclaimer of dead-incarnation `_files`
    objects, and it treats them exactly like ref debris: enumerated through
    `namespaceAllLivesPrefix`, deleted BY EXACT TOKEN, under the DEPOSITED incarnation.** LIST
    omission may only leak storage — never visibility, rebirth or deletion safety (directive §2). A
    `_files` object whose token changed under the janitor is RETAINED and surfaced, not deleted.
  - **The cleanup item carries the incarnation CAPTURED AT DEPOSITION; a resumed pass NEVER
    re-derives it from the catalog** (spec §3 bold text; `CaRefNsCleanupStaleLeaderCore`'s
    proven rule). Deposition writes the captured incarnation (capture-time correctness — the
    TLA Task 4 obligation), and the resumed-pass path has a test proving a reborn same-name
    namespace's data survives a stale cleanup resume.

- [ ] **Step 1: Failing tests**: full removal (Removing → terminal → fold → `_ckpt` exact-token
  delete → entry delete, in that order — assert order via backend op journal); terminal append
  refused for a non-owner without a claimed fence; GC observing terminal-less `Removing` for
  N rounds surfaces it and appends NOTHING; janitor deletes planted foreign-incarnation debris
  under suppression rules; the stale-cleanup-resume rebirth test (deposit cleanup for inc₁,
  drop + recreate ns as inc₂, resume the pass → inc₂'s objects untouched, inc₁ debris deleted);
  deposition-writes-captured-incarnation unit test (white-box: the deposited item's field
  equals the incarnation at deposit time even if the catalog changed before the write landed).
- [ ] **Step 1b (amendment): the `_files` half of the same guarantees**, in
  `gtest_cas_ns_removal_lifecycle.cpp`:
  `StaleCleanupResumeSparesRebornNamespaceFiles` — the directive's "stale cleanup resuming after a
  new incarnation exists" test, `_files` edition: plant `_files` objects under inc₁ AND inc₂,
  deposit the cleanup item for inc₁, resume the pass, assert inc₂'s files are byte-identical and
  inc₁'s are gone (this is the data-loss test — a re-derivation from the current catalog entry
  would delete inc₂'s live files);
  `NamespaceRemovalDoesNotListOrDeleteFiles` — removal's mandatory path issues NO `_files` LIST and
  NO `_files` delete (assert via the backend op journal), and the entry delete lands with `_files`
  objects still present;
  `JanitorRetainsFilesObjectWhoseTokenChanged` — token mismatch at the delete → retained + surfaced;
  `RemovalDriverResumesEntryDeleteAfterCkptGone` — the window bullet above: kill after the `_ckpt`
  delete, remount the owning writer, the entry is removed and no `_ckpt` is re-created.
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate + lanes green.
- [ ] **Step 5: Commit** `ca: ref — removal lifecycle: fenced terminal, immediate entry delete, deposited-incarnation cleanup`.

### Task 5b: `chooseRecoveryGrounding` — recovery becomes LIST-independent {#task-5b}

[Directive design change 3 + implementation improvement 2 = amendment commit 6. **This is the task
that kills `[RECOVER-REF-TABLE-LIST-RESIDUAL]`** — the BACKLOG residual Task 0 Step 3 registered as a
Task 7b PRECONDITION, guarded by two capstone sentinels deliberately written to go RED when it dies.
Recommended order puts Task 7 before this one so the `Removing`-without-`_ckpt` window always has an
owner (see the overview's scheduling notes).]

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
- Lifecycle rules, verbatim: "`Creating` namespaces are never recovered or published; `Live`
  namespaces require a readable `_ckpt` with `life_epoch`; ordinary `Removing` namespaces also require
  `_ckpt`; the special `Removing` + missing `_ckpt` finalization window is handled by
  removal/decommission logic, not recovery; missing required `_ckpt` is corruption."
- What LIST may STILL do, verbatim: "offer a newer snapshot candidate; provide additional diagnostic
  witnesses; nominate garbage for cleanup." What it may not: "determine genesis or committed history."
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
  `OrdinaryRemovingWithoutCkptIsCorruption`;
  `RemovingInFinalizationWindowIsRoutedToRemovalNotRecovered` (Task 5's window: recovery declines,
  the removal driver finishes it);
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

[Amendment commit 7's read/write half; the cleanup half is Task 5. This task owns the LIFE HANDLE
itself, which is why the namespace-file hot-path requirements land here and not in Task 4b: "hot
reads and writes use an already-held life handle" needs the handle to exist.]

**Files:**
- Modify: the ref read paths (`.../Pool/CasRefLedger.cpp` readers; the table-cache layer —
  `RefTableCacheEviction*` test family marks the surface), destructive cleanup sites from
  Stage A Task 9's list
- Modify (amendment, namespace files): `.../Pool/CasPlainObjects.cpp` hot read/write paths take the
  life from the caller's handle; `ContentAddressedMetadataStorage.cpp` — `namespaceFilesReadable`
  (`:1231`, body `return !store()->namespaceIsRemoved(ns);`) and its five call sites (`:1325` in
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
- Destructive cleanup revalidates life and fence IMMEDIATELY before every delete — and since
  the backend exposes only PER-KEY `deleteExact` (no atomic batch delete), "before every
  delete" means per key, not per planning batch [codex finding 14]: the janitor and
  `cleanupRefObjects` re-read the catalog entry token and re-check the fence before EACH
  `deleteExact`; the token/fence read may be cached only within a span where the check itself
  proves nothing destructive-relevant can have changed (in practice: re-read per key; if a
  measured hot spot ever demands batching, that is a protocol change needing its own review,
  not an optimization).

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
- **`namespaceFilesReadable`'s disposition, decided here and recorded by Task 9.** Its premise is
  two claims stacked: (a) it guards against reading a REBORN namespace's files, and (b) it makes a
  durably-`Removed` namespace's files read as absent before GC physically reclaims them. The re-key
  kills (a) — a life-keyed read cannot resolve into another life — but (b) SURVIVES: a reader holding
  the very life that was removed can still address those objects by key until cleanup runs. So the
  gate STAYS and its documentation (`ContentAddressedMetadataStorage.h:424-428`) is corrected to
  claim only (b); Constraint 3's "remove a branch whose premise died" applies to the RATIONALE, not
  to the check. If the implementer finds (b) also covered elsewhere, removing the gate is acceptable
  — but only with the proof written into the task report.

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

**Files:**
- Modify: `.../ContentAddressed/Tools/CasDecommission.cpp` (scoped-LIST discovery `~:116`
  replaced) [path per codex finding 18]
- Create: `src/Disks/tests/gtest_cas_decommission_catalog_duties.cpp`

**Interfaces (register R5 verbatim — same-rollout dependency of the catalog):**
After claiming the victim server root: enumerate its catalog entries EXACTLY (no LIST);
`Removing` without a terminal record = resumable writer work — `_ckpt` present → recover and
append the terminal under the claimed fence; `_ckpt` absent → the finalization window after
cleanup → exact-CAS-remove the catalog entry, else corruption; a FINAL exact catalog GET/token
check immediately before slot retirement; retirement FORBIDDEN while any entry owned by that
root remains.

[Amendment note: the `_ckpt`-absent branch becomes LOAD-BEARING rather than merely tidy. Task 5b
makes recovery REFUSE to ground a `Removing` entry with no `_ckpt`, so for a root that never comes
back this branch is the only thing that finishes the removal — which is why the recommended order
schedules this task BEFORE Task 5b. It must not re-create `_ckpt`, and "else corruption" keeps its
meaning: an absent `_ckpt` outside the finalization window is still corruption, not a licence to
invent one.]

- [ ] **Step 1: Failing tests**: hidden `Removing` entry (LIST would have missed it; catalog
  does not) blocks slot retirement; the `_ckpt`-present resumption appends the terminal under
  the claimed fence and completes removal; the `_ckpt`-absent finalization branch; the
  token-changed-at-final-check race → retirement refused, retried; retirement with zero owned
  entries proceeds.
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate +
  `test_content_addressed_drop_pool_member` lane green.
- [ ] **Step 5: Commit** `ca: decommission — catalog-exact duties; retirement fenced on owned entries`.

### Task 7a: delete probe A — the second full ref LIST goes {#task-7a}

[2026-07-30 GC directive step 1: "Удалить probe A. Удалить дополнительный LIST, setting, counters,
phase, tests и устаревшие комментарии. Сохранить B1/B2 и mount-time capability probe."
**Rationale — why it goes rather than stays:** probe A is a sampled store-quality detector whose
entire signal is "a LIST can be a liar". Under a catalog-authoritative universe (Task 4) and
LIST-independent recovery (Task 5b), no correctness decision rests on LIST fidelity any more — the
detector measures a property nothing depends on, and it pays one extra FULL enumeration of
`cas/refs/` per sampled round to do it. Register R7 ("Probe A gating policy — DECIDED and EXECUTED")
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
  two callers — `Gc::listRefPrefix` (`:3501`, "the round's ONE full enumeration of `cas/refs/`",
  comment at `:390`) and the detector's `probe_scan = enumerateRefPrefix()` (`:3570`). Task 4 already
  replaced `discoverUniverse`'s enumeration with one catalog `GET`, so after this deletion the helper
  has a single caller. Decide and record: collapse it into `listRefPrefix` if the split existed only
  to give the detector a second, independent enumeration, or keep it with a comment saying what else
  justifies it. Do not leave the question unasked.
- [ ] **Step 3: the result criterion, asserted** (directive: "после удаления probe A нет второго
  полного ref LIST"): the CONVERTED test (iii) above asserts a folding round performs EXACTLY ONE full
  enumeration of `cas/refs/` — and, critically, on EVERY round including the ones that used to be
  sampled (with the old cadence, round 16 would have been the one to catch this; a test that only
  checks round 1 proves nothing). Assert by LIST count attributed to the ref prefix, not by wall time.
  The claim is verifiable rather than merely plausible because `Gc::enumerateRefPrefix`
  (`CasGc.cpp:3462`) has exactly two callers tree-wide — `listRefPrefix` (`:3505`, the round's own
  scan) and the detector (`:3570`) — and the round's other `backend.list` calls (`:3004`, `:3147`,
  `:3263`) target manifest and generation prefixes, not `cas/refs/`. Re-verify that call-site count in
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

**PLACED HERE BY THE PLACEMENT SWEEP (2026-07-30), previously an unplaced deferral:**
`[CKPT-DAMAGE-NO-REPAIR-PATH]` residual (a) — a single unrepaired `_ckpt` HOLDS its namespace and
therefore shuts the ROUND-WIDE destructive gate, so one damaged 4 KiB object stops ALL reclamation
pool-wide. The BACKLOG entry names the code comment that "names the set the flip must carry"
(`CasGc.cpp:2515`), and this is the task that narrows that gate — so the narrowing must CARRY the
hold set, or this flip ships the very stall it was meant to relieve. Concretely: when `kDefault`
becomes authoritative, a held namespace must suppress destruction FOR ITSELF and not for the round,
and the anomaly-arm term (the undecodable-`_ckpt` namespace with no walk position, which mints no
hold by design) must be carried too — those are the TWO things the `CasGc.h` "MUST CARRY TWO THINGS"
paragraph enumerates. Assert both: a pool with one held namespace still reclaims in the others.

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
  (a) unqualified file keys aliasing a reborn namespace → closed by Task 4b's re-key (name its
  test);
  (b) the `namespaceFilesReadable` TOCTOU (`ContentAddressedMetadataStorage.cpp:1231`) → record Task
  6's split verdict: the rebirth arm of its premise is dead (a life-keyed read cannot resolve into
  another life), the removed-but-not-yet-reclaimed arm survives, so the gate stays with corrected
  documentation — or, if Task 6 removed it, the proof it recorded;
  (c) the LIST-derived physical-empty proof (`Gc/CasGc.cpp:2998-3010`
  `namespacePhysicallyEmpty`) → its `_files` arm deleted by Task 4b (rebirth-does-not-wait), `_files`
  out of the mandatory removal passes (`:3135-3138`), omission leak-only;
  (d) migration → format bump B, Constraint 14.
- [ ] **Step 2 (the one open question):** Determine whether LOOSE MOUNTPOINT OBJECTS carry a
  rebirth-aliasing hazard of their own. They are outside namespace ownership and the directive
  keeps them unqualified, so the answer is expected to be no — but ANSWER it from the code, do not
  assume: if a mountpoint object's identity can be re-resolved by a same-name reborn namespace,
  that is a surviving hazard, it is OUT of this amendment's scope, and it gets a new register item
  + a `BACKLOG.md` entry naming the exposure (and only then a small spec, as its own unit of work).
- [ ] **Step 3:** Commit `ca: docs — R1 verbatim-file aliasing closed by the namespace-life re-key`.

### Task 10: TLA debt from the phase — FOUR sub-tasks, each its own review unit {#task-10}

[Codex finding 17: the original single-unit framing could hide an evidence-sensitive model
retirement inside a mechanically enormous diff. Each sub-task below is dispatched, reviewed and
committed INDEPENDENTLY, with its own before/after results artifact.]

**Task 10a — `listedTok` semantic audit.** Audit `CaGcRootLocalPartManifestCore`'s `listedTok`
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

**Task 10d — phase-runner classifier.** Fix the five phase runners' inert `[A-Za-z_]+` →
`[A-Za-z0-9_]+`; re-run all five end-to-end once (expect identical results; paste tails).
Commit alone; final commit updates `models/README.md` + `cas/06-tla-models.md`.

### Task 11: Stage B gates {#task-11}

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
- [ ] **Step 3:** Soak battery, all three REQUIRED green:
  (a) churn soak — create/drop namespaces at ≥1/s for ≥30 min under load: catalog entry count
  returns to baseline (O(Creating+Live+Removing) — the r9-6 flatness claim), zero alias reads,
  fsck clean; (b) rebirth adversarial scenario — drop/recreate under concurrent readers +
  stale cleanup resume (Task 5's test at soak scale), and after the amendment the readers must
  include NAMESPACE-FILE readers/writers, not only ref readers: zero reads resolving to a newer
  incarnation across the whole run, `_files` debris from dead incarnations trending to zero via the
  janitor without ever blocking a rebirth; (c) decommission scenario — victim with
  hidden `Removing` entries drained correctly (Task 7 at soak scale); plus phase 3
  `--duration 90m` general soak, same PASS criteria as Stage A.
- [ ] **Step 3c (2026-07-30 GC directive): THE SEQUENTIAL-BASELINE DESTRUCTIVE SOAK.** This is a
  distinct, REQUIRED soak, not a variant of (a)-(c): run the destructive round on **the current
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
  `namespace_cleanup` (`items`/`items_pending`/`items_completed`). **Generation pruning is the
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
  | 5 | After probe-A removal there is no second full ref LIST | LIST counts per round attributed by prefix | exactly ONE full `cas/refs/` enumeration per round, on EVERY round including those probe A used to sample |
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
   `listedTok` audit + drivers + runnerless + classifier → T10a-d; destruction enablement
   ordering → T7b.
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
   and Task 5's chartered all-lives LIST helper is ONE helper for both object families, not two.
