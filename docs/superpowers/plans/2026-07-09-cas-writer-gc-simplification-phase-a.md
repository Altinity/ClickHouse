# CAS writer↔GC simplification, Phase A — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **Task 1 (TLA+ Gate A) is CONTROLLER-INLINE** (project precedent for TLA+ gates) — no code task starts before the gate is green.

**Goal:** Remove the redundant writer-side freshness machinery (promote tokened revalidation, retained sources, copy-forward pre-pass, `view_gate` drain, writer fence-refresh, dead fields) on the strength of the consult-verified EDGE-BEFORE-OBSERVE theorem, keeping the safety-critical dedup-adoption gate, the owner check, and the single non-tokened presence observation.

**Architecture:** Spec `docs/superpowers/specs/2026-07-09-cas-writer-gc-simplification-design.md` (Part I + Deletions D1-D8 + Kept K1-K6 + Gate A). All changes are writer-side (`Core/CasBuild.{h,cpp}`, `Core/CasStore.{h,cpp}`) + test migration. Phase B (meta-descriptor) is a separate later plan.

**Tech Stack:** C++ (CAS core), GoogleTest (`build/src/unit_tests_dbms`), TLA+/TLC (`docs/superpowers/models/`, `tmp/tla2tools.jar` via each model dir's run script).

## Global Constraints

- Branch `cas-gc-rebuild`; new commits only — never rebase/amend.
- Allman braces; no `sleep` for synchronization; runtime errors = `ErrorCodes::ABORTED`, never `LOGICAL_ERROR`.
- **No code before TLA+ Gate A is green.** D5 (Task 8) executes only if its gate config flipped green.
- Build FOREGROUND: `ninja` blocking, output redirected to `build/*.log`, NO `-j`/`nproc`; unit binary `build/src/unit_tests_dbms`; use a subagent to summarize build logs.
- Known-pre-existing gtest failures (NOT regressions; must not grow): the flaky `CaWiring*` GC/shadow set (`FreezeViaHardLinksIntoShadow`, `DisplacedTreeBlobsReclaimedThroughRealPath`, `DroppedPartIsReclaimedByRounds`).
- Commit trailers on every commit:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`

---

### Task 1: TLA+ Gate A (CONTROLLER-INLINE)

**Files:**
- Create: `docs/superpowers/models/CaEdgeBeforeObserve.tla` + configs `CaEdgeBeforeObserve_reduced.cfg`, `_sab_late_edge.cfg`, `_sab_no_adopt_check.cfg`, `_sab_no_k3_head.cfg`, `_sab_no_k3_adopt_check.cfg`
- Reuse: `docs/superpowers/models/run_tlc.sh` pattern (copy as `run_ebo.sh` targeting the new module)

**Interfaces:**
- Produces: the gate verdict authorizing Tasks 2-9, and the D5 decision (Task 8 go/no-go).

**Model specification** (transcribe to TLA+; keep the state space tiny — 1-2 writers, 2-3 hashes, 1-2 shards, bounded rounds):

- State: `present[h]` (body), `condemnedEntry[h]` (none | {round, pending}) — GC's retired ledger; `installedView[w]` (round; entries visible = all condemnedEntry with round ≤ installedView, entries persist until confirmed), `advertised[w]` (= installedView, monotone), `edges` (durable closure sets per build with an appended-at timestamp vs per-shard seal), `committed` refs, per-shard `seal` taken at pass start for that shard (finding E), GC `round`.
- Actions: `WriterPrecommit(b)` (durable closure naming the build's hashes); `WriterAdopt(b,h)` — enabled only post-precommit (order), observes `present[h]`, consults the view iff `AdoptCheck` (K1: condemned-in-view ⇒ displace = fresh incarnation, drops the entry's applicability); `WriterFreshUpload(b,h)`; `WriterPromote(b)` — commits; with `K3Head` HEADs non-tokened leaves (absent ⇒ abort), with `K3AdoptCheck` consults the view for tokenless leaves; NO tokened revalidation (the reduction under test); `ViewInstall(w)` (advance installedView to current published round); `GcPass` — per-shard seal, fold (edges appended before that shard's seal count), settle (d>0 ⇒ spare/drop entry incl. pending — loud if pending, d=0 ⇒ graduate if `condemn_round < min(advertised)`, pending+d=0 ⇒ execute delete: `present[h] := FALSE`), publish round+1.
- Invariants: `INV_NO_DANGLE` (committed ref ⇒ all its hashes present), `TypeOK`.
- Constants (flags): `OrderSabotage` (adopt enabled pre-precommit), `AdoptCheck`, `K3Head`, `K3AdoptCheck`, `Drain` (when FALSE, `advertised` may advance mid-writer-action — model the D4 removal).

**Config matrix (expected outcomes — the gate):**

| Config | Flags | Expected |
|---|---|---|
| `_reduced` | order on, AdoptCheck on, K3 on, Drain OFF | **GREEN** (positive: reduced protocol sound) |
| `_sab_late_edge` | OrderSabotage | **RED** (dangle — ordering is load-bearing) |
| `_sab_no_adopt_check` | AdoptCheck off | **RED** via the K1 pre-graduated interleaving |
| `_sab_no_k3_head` | K3Head off | **RED** (tokenless/no-dep absent leaf dangles) |
| `_sab_no_k3_adopt_check` | K3AdoptCheck off, K3Head on | **RED** (finding C: the promote-adopt shape) |

- [ ] **Step 1:** Write the model + configs per the specification above.
- [ ] **Step 2:** Run all five: `./run_ebo.sh <cfg>` (TLC, `-Xmx8g`). Record state counts + verdicts.
- [ ] **Step 3 (D5 decision):** Re-run the shard-incarnation newborn sabotage family (`docs/superpowers/models/CaGcShardIncarnationCore_sab_newbornnofloor.cfg` baseline) and evaluate whether the writer-side fence-refresh removal (D5) keeps it green in the reduced-writer reading (consult verified THM-NO-RETURN redundancy; this step is the formal confirmation). If NOT confirmed → D5 CANCELLED (skip Task 8), record in the spec.
- [ ] **Step 4:** Commit models + a gate-results note in the worklog: `docs(cas): TLA+ Gate A — EDGE-BEFORE-OBSERVE reduced model green; sabotages red; D5 <verdict>`.

---

### Task 2: D1+D2 — reduce the promote revalidation loop; remove retained sources

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (the revalidation loop inside `Build::promote`'s `mutateShard` closure; `putBlob`'s `retained_sources.insert_or_assign` block; `retainedSourceFor` definition)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h` (`retained_sources` member + comment; `retainedSourceFor` decl)

**Interfaces:**
- Consumes: `depIsTokened(const UInt128&) const` (exists, `CasBuild.cpp:~228`) — becomes the loop boundary; `isCopyForwardableTokenless`, `copyForwardFromCondemned` (unchanged).
- Produces: the reduced loop below; `retained_sources`/`retainedSourceFor` gone.

- [ ] **Step 1: Write the failing test** — Task 6's `PromoteIgnoresCondemnedTokenedBlobEdgeProtected` (promote succeeds AND the blob's token is UNCHANGED — no resurrect PUT). Against current code it FAILS (the current loop re-uploads: token changes). Build + run to confirm RED.
- [ ] **Step 2: Replace the revalidation loop** with:

```cpp
        /// Blob-leaf revalidation (spec 2026-07-09-cas-writer-gc-simplification, Phase A): TOKENED leaves
        /// are edge-protected — EDGE-BEFORE-OBSERVE: the precommit closure was durable BEFORE putBlob
        /// observed them, so a condemnation in the putBlob→promote window cannot graduate (the next fold
        /// sees the edge, d >= 1, spared), and putBlob's gate already validated them against the installed
        /// view under that edge. They are NOT re-checked here. Every NON-tokened leaf (a tokenless
        /// W-EVIDENCE adopt — observation-free by design, B188 — or a no-dep staging bug) gets the single
        /// mandatory presence observation: absent => fail closed; condemned-present + copy-forwardable =>
        /// verified copy-forward (the INV-1 exception); condemned + no-dep => fail closed.
        for (const ManifestEntry & e : body.entries)
        {
            if (e.placement != EntryPlacement::Blob)
                continue;
            if (depIsTokened(e.blob_hash))
                continue;   /// edge-protected (EDGE-BEFORE-OBSERVE); putBlob validated under the durable edge
            const BlobId blob_id{u128ToHex(e.blob_hash)};
            const String blob_key = store->layout().blobKey(blob_id);
            constexpr int max_reval_attempts = 8;
            bool validated = false;
            for (int attempt = 0; attempt < max_reval_attempts; ++attempt)
            {
                const HeadResult hr = store->backend().head(blob_key);
                if (!hr.exists)
                    throw Exception(ErrorCodes::ABORTED,
                        "promote: blob {} absent at commit revalidation — failing closed", blob_key);
                if (store->retireView().isCondemnedToken(ObjectKind::Blob, e.blob_hash, hr.token))
                {
                    if (!isCopyForwardableTokenless(e.blob_hash))
                        throw Exception(ErrorCodes::ABORTED,
                            "promote: blob {} condemned at commit revalidation — failing closed (INV-1)", blob_key);
                    copyForwardFromCondemned(e.blob_hash, blob_key, hr);
                    continue;
                }
                validated = true;
                break;
            }
            if (!validated)
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} still condemned after {} copy-forward attempts at commit revalidation — "
                    "failing closed (INV-1)", blob_key, max_reval_attempts);
        }
```

- [ ] **Step 3: Remove retained sources.** Delete from `CasBuild.cpp`: the `putBlob` block `retained_sources.insert_or_assign(logical_hash, source);` with its comment (~137-139); the `retainedSourceFor` definition (~216-221). Delete from `CasBuild.h`: the `retained_sources` member + its doc comment; the `retainedSourceFor` declaration.
- [ ] **Step 4:** `cd build && ninja unit_tests_dbms > build_pa_task2.log 2>&1` → run `--gtest_filter='*PromoteIgnoresCondemnedTokenedBlobEdgeProtected*'` → PASS. (Other promote tests will fail until Task 6 migrates them — expected; record the list.)
- [ ] **Step 5: Commit** — `refactor(cas): promote skips tokened leaves (EDGE-BEFORE-OBSERVE); drop retained sources (D1+D2)`.

---

### Task 3: D6+D7 — remove `observed_view_round` and `hasDep`

**Files:** `Core/CasBuild.h` (`DepEntry::observed_view_round` at ~110; `hasDep` decl), `Core/CasBuild.cpp` (FOUR positional aggregate-init sites for `DepEntry` at ~313, ~388, ~628, ~636 — finding D; `hasDep` definition ~234-241), plus any test callers of `hasDep` (grep; rewrite or drop).

- [ ] **Step 1:** Remove the field; fix all four `DepEntry{...}` inits (drop the positional zero). Remove `hasDep` (verify `grep -rn "hasDep" src/` shows only tests → migrate them to `depIsTokened` or direct expectations).
- [ ] **Step 2:** Build + run `--gtest_filter='Ca*:*Cas*'` (log to `tmp/pa_task3_gtests.log`); failure set unchanged vs Task 2's recorded list.
- [ ] **Step 3: Commit** — `refactor(cas): drop dead DepEntry::observed_view_round and hasDep (D6+D7)`.

---

### Task 4: D3 — delete the copy-forward pre-pass

**Files:** `Core/CasBuild.cpp` — the pre-pass block in `Build::promote` (marked by the `/// Copy-forward pre-pass` comment, currently ~816-836) including its loop; keep `isCopyForwardableTokenless` (used by Task 2's loop) and `copyForwardFromCondemned` (single caller remains).

- [ ] **Step 1:** Delete the block; replace with a two-line comment: the in-closure gate is now the single copy-forward site (spec D3; trade-off noted — rare in-closure GET+PUT, idempotent under CAS retry).
- [ ] **Step 2:** Build + targeted run of the copy-forward tests (`*CopiesForward*`, `*EvidenceHit*`): the in-closure backstop covers what the pre-pass did — `PromoteCopiesForwardCondemnedEvidenceRevealedAfterRefresh` and the pre-pass-shaped test (`CondemnedPresentEvidenceDepCopiesForwardAtGate` — now exercises the same in-closure path) must PASS.
- [ ] **Step 3: Commit** — `refactor(cas): drop the promote copy-forward pre-pass; in-closure gate is the single site (D3)`.

---

### Task 5: The ordering guards (chassert + comments)

**Files:** `Core/CasBuild.cpp` (`Build::observeAndAdmit` both overloads; `Build::putBlob` header comment), `ContentAddressedTransaction.cpp` (`publishStaging` precommit site comment).

- [ ] **Step 1:** At the top of BOTH `observeAndAdmit` overloads add:

```cpp
    /// EDGE-BEFORE-OBSERVE (spec 2026-07-09-cas-writer-gc-simplification): adopting an EXISTING
    /// incarnation is safe ONLY under this build's durable precommit closure — an adopted blob carries the
    /// ORIGINAL writer's build_id, so the newborn-debris watermark does not cover it. Fresh uploads
    /// pre-precommit stay legal (watermark-protected). See the TLA+ order sabotage (Gate A).
    chassert(precommitted);
```

- [ ] **Step 2:** Add the spec-reference comments at `publishStaging`'s `precommitAdd` call and `putBlob`'s doc block.
- [ ] **Step 3:** Build + full `Ca*:*Cas*` sweep: any test tripping the chassert is an old-order dedup-adopt — reorder that test to the wiring order (`stageManifest → precommitAdd → putBlob`). Record which tests were reordered.
- [ ] **Step 4: Commit** — `feat(cas): assert adopt-under-durable-edge ordering (EDGE-BEFORE-OBSERVE guard)`.

---

### Task 6: Test migration to the Phase-A contract

**Files:** `src/Disks/tests/gtest_ca_wiring.cpp`, `gtest_cas_protocol_scenarios.cpp`, `gtest_cas_build.cpp`.

Per the spec's Phase-A list:
- [ ] `CaWiringResurrect.PromoteResurrectsCondemnedTokenedBlob` → `PromoteIgnoresCondemnedTokenedBlobEdgeProtected` (written RED in Task 2): promote succeeds, ref resolves, **token unchanged** (`EXPECT_EQ(head(blob_key).token, t0)`).
- [ ] `PromoteAbandonedPrecommitAbortsWithoutResurrect` → keep; assert only owner-check ABORTED + token unchanged.
- [ ] `CasProtocol.FenceConflictCondemnedTokenedBlobResurrectsFromSource` → `…CommitsWithTokenUnchanged` (success, no re-upload); same for `WedgedHeartbeat…`; `RevalidateReObservesStaleTokenKeepsWhenUnchanged` → simplify (tokened leaves are not re-observed at all — assert commit + token unchanged); `RevalidateAdoptsLiveTokenWhenOnlyPhantomCondemnedAtDifferentToken` → putBlob-level behavior, keep with order fixed if needed.
- [ ] `CasProtocol.RevalidateAbsentTokenedBlobResurrectsFromSource` → **DELETE** + file-header note: a hand-deleted `putBlob`'d body is protocol-unreachable under EDGE-BEFORE-OBSERVE (out-of-band corruption = `ca-fsck`'s domain).
- [ ] Tokenless/no-dep tests kept as-is and must stay green: `EvidenceHitCondemnedPresentBlobCopiesForwardInClosure`, `PromoteCopiesForwardCondemnedEvidenceRevealedAfterRefresh`, `PromoteAbsentTokenlessBlobAbortsRetryable`, `PromoteCondemnedLeafWithoutDepAbortsFailClosed`.
- [ ] Full sweep `--gtest_filter='Ca*:*Cas*'` → failure set = the known flaky `CaWiring*` set only.
- [ ] **Commit** — `test(cas): migrate promote tests to the Phase-A contract (tokened leaves edge-protected)`.

---

### Task 7: D4 — remove the `view_gate` drain

**Files:** `Core/CasStore.h` (member `view_gate` + comment block ~622-626; the drain doc ~402-408), `Core/CasStore.cpp` (`syncRetiredView`'s `std::unique_lock` drain ~690; `flushShardBatch`'s `std::shared_lock` ~1243 + comment ~1241-1242), `Core/CasBuild.cpp` (the ~818 comment referencing the drain).

- [ ] **Step 1:** Delete the member and both lock sites; the syncer installs under `RetireView`'s internal mutex alone. Replace the comments with the corrected justification (spec D4 / finding B): entry persistence + monotone installs make any in-closure view read see every graduated entry regardless of drain; displacement is exact-token-safe.
- [ ] **Step 2:** Build + full sweep + specifically the concurrency-adjacent tests (`*RetireView*`, `*Sync*`); failure set unchanged.
- [ ] **Step 3: Commit** — `refactor(cas): drop the view_gate install drain (D4)`.

---

### Task 8: D5 — remove the writer fence-refresh (ONLY if Gate A Step 3 confirmed)

**Files:** `Core/CasBuild.cpp` ~856-857 (`if (store->retireView().round() < root.fence_round) store->retireView().refresh();` + its comment block).

- [ ] **Step 1:** Delete the refresh + comment; note GC-side `fence_round` uses stay (round recovery, birth floor install, codec/inspect).
- [ ] **Step 2:** Build + sweep (esp. `CasGcShardIncarnation*` tests) → unchanged.
- [ ] **Step 3: Commit** — `refactor(cas): drop the writer-side fence_round view refresh (D5, TLA+-gated)`.

---

### Task 9: Validation + bookkeeping

- [ ] Full `Ca*:*Cas*` sweep (record counts; flaky set not grown).
- [ ] Rebuild `clickhouse` (foreground, logged) → run the 4 condemn-race stateless tests on the CA-s3 lane (`python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed s3 storage, parallel)" --test 01156_pcg_deserialization 01710_projection_detach_part 02346_exclude_materialize_skip_indexes_on_insert 03283_optimize_on_insert_level`) → all OK.
- [ ] Full CA-s3 lane → 0 promote aborts, failure set = known env set; then `ca-fsck` on the lane pool → 0 dangles.
- [ ] D8: backlog supersede notes (already partially done); worklog + `.superpowers/sdd/progress.md` updates.
- [ ] **Soak with the fsck gate** (the Phase-A exit criterion before Phase B) — launched as a separate run; not part of this plan's commits.

---

## Self-Review

**Spec coverage:** D1→T2, D2→T2, D3→T4, D4→T7, D5→T8 (gated), D6+D7→T3, D8→T9; K1/K2/K3 untouched-by-construction (T2 keeps the non-tokened loop verbatim semantics); ordering guards→T5; Gate A→T1 incl. finding-C config; test list→T6 matches the spec's enumeration. Phase B intentionally absent (separate plan after meta-consult).

**Placeholder scan:** T1 carries a full model/config specification (transcription task, controller-inline per precedent) — no TBDs. T2 carries the complete replacement loop. T3/T4/T5/T7/T8 are precise deletions with anchors + literal insertions where code is added.

**Type consistency:** `depIsTokened(const UInt128&)`, `isCopyForwardableTokenless(const UInt128&)`, `copyForwardFromCondemned(const UInt128&, const String&, HeadResult)` — all existing signatures; the loop uses them unchanged.

## Execution Handoff

Task 1 controller-inline. Tasks 2-8: subagent-driven (T2+T6 share the RED test — may run as one implementer unit; T3/T4/T5 mechanical — cheap model; T7 needs care — standard model), task review after each unit, whole-branch review before Task 9's lane run.
