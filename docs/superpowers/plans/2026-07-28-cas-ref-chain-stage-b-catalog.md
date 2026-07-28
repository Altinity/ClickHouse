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

**Tech Stack:** identical to Stage A (same tree, same gates, same soak harness).

## Global Constraints {#global-constraints}

Constraints 1-11 of the Stage A plan
(`2026-07-28-cas-ref-chain-stage-a-streams.md {#global-constraints}`) apply VERBATIM to every
task here. Additional Stage-B constraints:

12. The incarnation qualifies THE REF LAYER ONLY (`<ns>/<inc>/{_log,_snap,_ckpt}`); manifests
    keep `(namespace, mount-epoch, build-sequence)` identity; verbatim FILES stay unqualified
    (their hazard is R1 — Task 9 writes its spec, nothing here touches file keys).
13. Catalog admission refuses loudly; removal is NEVER refused (spec INV-3).
14. Format bump B (Task 4) is Stage B's single recreate-only bump — one bump, all re-keying
    behind it.

## Task overview {#task-overview}

| # | Task | Source | Depends on |
|---|---|---|---|
| 0 | Stage-A gate preflight | — | — |
| 1 | `RefNamespaceId` type; namespace-only overloads deleted | §2 r9-3 | 0 |
| 2 | Catalog object: format, states, capacity admission | INV-3 | 0 |
| 3 | Creation lifecycle + three-site recheck completion | §3 | 1,2 |
| 4 | Re-key ref layer under `<ns>/<inc>/`; universe from catalog; format bump B | INV-3/§5 | 1,2,3 |
| 5 | Removal lifecycle: terminal record, janitor, deposited-incarnation cleanup | §3 | 3,4 |
| 6 | Read-side contract: handles + pre-delete life/fence revalidation | §2 | 4 |
| 7 | R5 decommission duties | register R5 | 4,5 |
| 8 | R2+R3: writer duty queue + orphan-blob nomination (one coherent change) + model extensions | register R2/R3, §9 | 4 |
| 9 | R1 spec (verbatim-file rebirth aliasing) — spec only | register R1 | — |
| 10 | TLA debt: `listedTok` audit, unasserted drivers, runnerless models, classifier | phase follow-ups | — |
| 11 | Stage B gates: battery + churn/rebirth/decommission soak + verdict | §9 | all |

Tasks 9 and 10 are independent of the code chain and may be scheduled at any point (still one
implementer at a time).

---

### Task 0: Stage-A gate preflight {#task-0}

- [ ] **Step 1:** `grep -n "STAGE A: PASS" docs/superpowers/cas/2026-07-28-stage-a-RESULTS.md`
  — exactly one match, else BLOCKED.
- [ ] **Step 2:** Re-run the CA gtest gate filter (Stage A Task 0's exact command); record
  baseline counts in the report. No commit.

### Task 1: `RefNamespaceId` — dropping the incarnation becomes unrepresentable {#task-1}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/RefNamespaceId.h`
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
  the namespace-only overload set no longer exists — since `refLogKey` is a MEMBER of
  `Layout`, express it as a dependent `requires`-expression concept check
  (`static_assert(!requires(const Layout & l, RootNamespace ns, RefTxnId id) { l.refLogKey(ns, id); });`)
  — the "cannot compile" half of spec §9 r9-5 #3 [codex finding 18].
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement type + re-plumb until the tree compiles —
  mechanical, compiler-driven; in Stage B Task 1 the incarnation VALUE everywhere is a
  placeholder constant from the not-yet-landed catalog: introduce
  `RefNamespaceId::stageATransition()` (a named constant incarnation) used ONLY by this task's
  plumbing and DELETED by Task 4 (grep-clean gate in Task 4's steps).
- [ ] **Step 4:** Full CA gate green. **Step 5: Commit**
  `ca: ref — RefNamespaceId{ns, incarnation}; namespace-only key helpers deleted`.

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
- Capacity admission (INV-3, both halves): namespace names get a byte bound
  (`kMaxNamespaceBytes = 512`); the creation CAS checks the ADDITIVE predicate — encoded
  catalog size + every entry's worst-case cursor/cleanup/hold reservation must fit BOTH the
  catalog object cap AND the fold-seal cap (Stage A Task 8's boundary arithmetic is the
  reservation's source of truth — reuse its constants, do not re-derive). Admission over the
  bound refuses loudly (`LIMIT_EXCEEDED` naming the namespace and both budgets); removal is
  never refused (Constraint 13).

- [ ] **Step 1: Failing tests**: codec round-trip all three states; strict rejections
  (duplicate ns; `creator` present on `Live`; `creator` absent on `Creating`; zero incarnation;
  non-canonical order; name over byte bound); token-CAS create/update/conflict-retry over
  InMemoryBackend; admission refused exactly AT the additive-predicate boundary and accepted
  one entry below (boundary + boundary-plus-one — Constraint 7); removal (`Live→Removing` CAS)
  succeeds at a full catalog (never refused).
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Full CA gate green.
- [ ] **Step 5: Commit** `ca: ref — ref_catalog object: states, incarnations, additive capacity admission`.

### Task 3: Creation lifecycle + the three-site recheck completion {#task-3}

**Files:**
- Modify: `.../Pool/CasRefCatalog.cpp` (+the lifecycle driver), the namespace-creation call
  path (today's `NamespaceBirth` writer — locate from `RefOpKind::NamespaceBirth` usages)
- Create: `src/Disks/tests/gtest_cas_ns_creation_lifecycle.cpp`

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
  Tests here: fence bump between `_ckpt` create and the `Creating → Live` CAS → refused by
  generation; entry token changed under the creator (concurrent reconciliation) → refused by
  token; both stale → refused (first check wins, either order acceptable).

- [ ] **Step 1: Failing tests**: happy path (3 writes, entry Live, `_ckpt` exists, incarnation
  stable across the sequence); crash after write 1 → reconciliation by a NEW actor refused
  while creator fence is live, succeeds token-exactly after fence terminal; stale token at
  reconciliation → fail closed; publication attempted while `Creating` → refused;
  fenced-out between `_ckpt` create and the `Creating→Live` CAS → the CAS refuses (the
  zombie-install C++ twin — `CaRefCatalogCore` `ZombieGoLive` red); the three
  fence-bump-between-sites tests.
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate green.
- [ ] **Step 5: Commit** `ca: ref — namespace creation lifecycle; token-exact reconciliation; three-site recheck`.

### Task 4: Re-key under `<ns>/<inc>/`; universe from catalog; format bump B {#task-4}

**Files:**
- Modify: `.../CasLayout.h` (the Task-1 helpers now take the incarnation into the KEY),
  `.../Pool/CasRefLedger.cpp` (writer + recovery paths carry `RefNamespaceId`),
  `.../Gc/CasGc.cpp` — `fold` universe (`ref_tables` seeding `:1049` region),
  `discoverUniverse` `:2393` (REPLACED: one catalog GET; the refs-prefix LIST becomes the
  intra-namespace hint only), REBUILD traversal, fsck universe; pool format bump B
- Create: `src/Disks/tests/gtest_cas_universe_from_catalog.cpp`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: spec §5's fold shape — per round ONE catalog `GET`; cursors keyed by catalog
  entries `(ns, incarnation)`; unhinted namespaces carry verbatim; the frontier proof (Stage A
  Task 9) now iterates EVERY `Live`/`Removing` entry — and with the universe authoritative,
  **this task flips Stage A's `kUniverseAuthoritative` to `true`, re-enabling production
  destruction** (the staging suppression's designed end): the Stage-A cross-namespace kill-shot
  test flips from "zero deletes because suppressed" to "zero deletes because namespace `A` is
  IN the catalog and its frontier is probed" — the same scenario now survives on proof, not on
  suppression. Recovery/fold/fsck/sweep construct `RefNamespaceId` from the catalog, live
  readers from their handle (§2). `RefNamespaceId::stageATransition()` is DELETED; a tree-wide
  grep for it must return zero (step-gated).

- [ ] **Step 1: Failing tests**: fold discovers a namespace with ZERO listable objects (hint
  fully blind) via the catalog and probes its frontier — the Stage-A residual test from
  `gtest_cas_list_liar_end_to_end.cpp` FLIPS from "documented gap" to "covered" (edit that test
  — the one intentional Stage-B edit of a Stage-A test, called out here so the reviewer expects
  it); a `Creating` entry is NOT folded, NOT frontier-required (no publication can exist);
  a `Removing` entry IS frontier-required; keys of a dead incarnation are refused by parsers
  (foreign-prefix inertness — the fold works only off catalog entries); old-format pool open →
  fail closed naming recreation (bump B).
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** Full CA gate + both CA-s3 lanes
  local green. **Step 5: Commit**
  `ca: ref — incarnation-keyed layer; universe from catalog; format bump B`.

### Task 5: Removal lifecycle — terminal record, janitor, deposited incarnation {#task-5}

**Files:**
- Modify: `.../Pool/CasRefCatalog.cpp`, `.../Pool/CasRefLedger.cpp` (terminal append path),
  `.../Gc/CasGc.cpp` (`runNamespaceCleanupPasses` `:2145` — the cleanup item shape), the
  namespace-removal call path (`RefOpKind::RemoveNamespace` usages)
- Create: `src/Disks/tests/gtest_cas_ns_removal_lifecycle.cpp`

**Interfaces:**
- Consumes: Tasks 2-4; Stage A holds (a `Removing` namespace can hold like any other).
- Produces (§3 verbatim):
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
    prefix).
  - Lazy janitor: whenever cleanup LISTING happens to return foreign-incarnation debris under a
    known namespace, delete it (omission = deferred cleanup, leak-only direction) — implemented
    inside `runNamespaceCleanupPasses`, gated by `suppress_destructive` like every destructive
    site.
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
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate + lanes green.
- [ ] **Step 5: Commit** `ca: ref — removal lifecycle: fenced terminal, immediate entry delete, deposited-incarnation cleanup`.

### Task 6: Read-side contract {#task-6}

**Files:**
- Modify: the ref read paths (`.../Pool/CasRefLedger.cpp` readers; the table-cache layer —
  `RefTableCacheEviction*` test family marks the surface), destructive cleanup sites from
  Stage A Task 9's list
- Create: `src/Disks/tests/gtest_cas_ref_read_contract.cpp`

**Interfaces (§2 verbatim):**
- Live readers hold `RefNamespaceId` handles and can never alias a new life (foreign prefix);
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

- [ ] **Step 1: Failing tests**: reader holding inc₁'s handle after drop+rebirth reads
  stale-or-`NotFound`, never inc₂ data (the alias test); hot read path performs ZERO catalog
  requests (op-count assert); entry token changed between plan and the FIRST delete → nothing
  deleted; token changed BETWEEN two keys of one cleanup pass → the first key's delete lands,
  the second is refused (the per-key revalidation race — codex finding 14).
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate green.
- [ ] **Step 5: Commit** `ca: ref — read-side contract: handle-scoped reads, pre-delete life revalidation`.

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

- [ ] **Step 1: Failing tests**: hidden `Removing` entry (LIST would have missed it; catalog
  does not) blocks slot retirement; the `_ckpt`-present resumption appends the terminal under
  the claimed fence and completes removal; the `_ckpt`-absent finalization branch; the
  token-changed-at-final-check race → retirement refused, retried; retirement with zero owned
  entries proceeds.
- [ ] **Step 2:** → FAIL. **Step 3:** Implement. **Step 4:** CA gate +
  `test_content_addressed_drop_pool_member` lane green.
- [ ] **Step 5: Commit** `ca: decommission — catalog-exact duties; retirement fenced on owned entries`.

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

### Task 9: R1 spec — verbatim-file rebirth aliasing {#task-9}

**Files:**
- Create: `docs/superpowers/specs/2026-07-XX-cas-verbatim-file-life-gate-design.md` (date at
  execution)

- [ ] **Step 1:** Write the SMALL spec (register R1's direction): qualify the file layer by
  incarnation OR a read-side life gate; must cover the `namespaceFilesReadable` TOCTOU
  (`ContentAddressedMetadataStorage.cpp` `~:1234`), the `_cleanup` LIST-derived
  physical-empty proof, and the migration story (recreate-only). 2-3 pages, alternatives table,
  falsification conditions per proposal — the house spec style.
- [ ] **Step 2:** Commit `ca: specs — verbatim-file life gate (R1) draft for review`. The spec
  goes to the user for review — implementation is OUT of this plan.

### Task 10: TLA debt from the phase — FOUR sub-tasks, each its own review unit {#task-10}

[Codex finding 17: the original single-unit framing could hide an evidence-sensitive model
retirement inside a mechanically enormous diff. Each sub-task below is dispatched, reviewed and
committed INDEPENDENTLY, with its own before/after results artifact.]

**Task 10a — `listedTok` semantic audit.** Audit `CaGcRootLocalPartManifestCore`'s `listedTok`
(`:79`) + the skip gate (`:866`) against v9: does the model's "discovery observes from LIST"
premise survive universe-from-catalog (Stage B Task 4)? Verdict into the model's RESULTS file;
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

- [ ] **Step 1:** Full CA gtest gate vs Task 0 baseline.
- [ ] **Step 2:** All CA integration lanes local (Stage A Task 14's list) green.
- [ ] **Step 3:** Soak battery, all three REQUIRED green:
  (a) churn soak — create/drop namespaces at ≥1/s for ≥30 min under load: catalog entry count
  returns to baseline (O(Creating+Live+Removing) — the r9-6 flatness claim), zero alias reads,
  fsck clean; (b) rebirth adversarial scenario — drop/recreate under concurrent readers +
  stale cleanup resume (Task 5's test at soak scale); (c) decommission scenario — victim with
  hidden `Removing` entries drained correctly (Task 7 at soak scale); plus phase 3
  `--duration 90m` general soak, same PASS criteria as Stage A.
- [ ] **Step 4:** Write the results file: battery table + `STAGE B: PASS`/`FAIL` verdict line +
  the post-B residual list (what remains open: R1 implementation, R4 registry, head-CAS north
  star §10).
- [ ] **Step 5: Commit** `ca: stage B — gate battery results + verdict`.

---

## Self-review checklist {#self-review}

1. Spec coverage: INV-3 → T2/T4/T5; §2 read contract + r9-3 → T1/T6; §3 creation → T3, removal
   → T5, recovery-ownership generation → carried from Stage A T6 (unchanged here); catalog
   capacity both halves → T2 (entry/byte) reusing Stage A T8 arithmetic; §5 catalog-driven
   universe → T4; register: R2/R3 → T8, R5 → T7, R1 → T9 (spec only), R6 accepted (no task —
   by design), R7 done in Stage A T12, R8 done during the phase.
2. Ledger obligations mapped: token-exact reconciliation at call site → T3; creator install
   generation+token → T3; three-site same-value → T3 (completing Stage A's two sites);
   deposited incarnation + capture-time correctness → T5; `listedTok` audit + drivers +
   runnerless + classifier → T10.
3. Every task name-checks its TLA counterpart where one exists; model edits follow phase
   conventions and are grouped in T8 (register models) and T10 (debt) — the five PHASE models
   stay sealed except T10's classifier-only runner fix.
