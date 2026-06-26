---
description: "Optional retire-token optimization for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 5 (retire-token)"
sidebar_position: 10
slug: /superpowers/plans/2026-06-26-cas-gc-phase5-retire-token-opt
title: "Phase 5 — Retire-Token Optimization — Implementation Plan"
doc_type: reference
---

# Phase 5 — Retire-Token Optimization — Implementation Plan {#phase-5-retire-token-optimization-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. Read `2026-06-26-cas-gc-redesign-overview.md` first, then `2026-06-26-cas-gc-phase0-tla-model.md` (the model STYLE TEMPLATE this plan extends). This is the **last and OPTIONAL** phase.

**This phase is OPTIONAL. Implement it only if the model gate in Task 1 passes.** It is a pure performance optimization with no behavior change visible to writers or readers: it removes the per-candidate `HEAD` in `retire` and instead sources the exact incarnation token from sealed generation state. The exact-token delete (`deleteExact`) authority is unchanged. If Task 1's model extension cannot be made green, **stop and surface** — do not write any code in Tasks 2-4.

**Goal:** Prove (Task 1) and then implement (Tasks 2-4) that `retire` can drop the `backend.head` it issues per zero-in-degree candidate — `Core/CasGc.cpp:1007` — and instead write `RetiredEntry.token` from a token observed and stored in sealed generation state at fold/retire time. The single destructive site, `deleteExact` at `Core/CasGc.cpp:382`, keeps carrying an **exact** token; the only change is where that token comes from. The proof obligation is sharp: a stale stored token can only **fail** the exact-token match (an UNDER-delete that spares live bytes), and can **never** match a re-incarnated object (an OVER-delete that loses live bytes).

**Architecture:** A TLC stage `EnableRetireTokenSource` added to `CaGcRootLocalPartManifestCore.tla`, branched from the Phase-0 model. In the baseline (flag FALSE) `GRetire` records `tokOf[h]` observed at retire time (the modeled per-candidate `HEAD`). With the flag TRUE, `GRetire` instead reads the token from a `storedTok` source written when the in-degree edge was last sealed, and the delete (`GRecheckDelete`/`Land`) still matches exactly against `tokOf[h]`. The negative control `SabotageStaleTokenOverDelete` accepts a stored token that is no longer current as a delete authority (a non-exact delete keyed on the stored value) and **must** violate `INV_NO_RETURN`. On the C++ side, retire sources `RetiredEntry.token` from the sealed in-degree/generation state instead of `backend.head`, then `recheck` deletes exactly as before.

**Tech stack:** TLA+ / TLC (`tla2tools.jar` at `tmp/tla2tools.jar`, OpenJDK 21) for Task 1; C++ (ClickHouse coding standards, Allman braces) + gtest (`unit_tests_dbms`) for Tasks 2-4.

## Global Constraints {#global-constraints}

*This section is repeated verbatim across every phase plan so an implementer who only sees one task still has them.*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in this phase may begin until Task 1's TLA+ gate is green.
- **R1 — bounded streaming round.** Work is proportional to changed owner transitions and the entries of the manifests they name; memory is bounded by stream buffers; GC state is coarse write-once objects.
- **R2 — target-shardable.** Default `gc_shards = 1`; sharded mode is optional (Phase 4).
- **R3 — simple, debuggable, idempotent, resumable.** Durable state explains what each round folded, retired, fenced, rechecked, deleted, trimmed.

**CA is pre-release**
- **ZERO on-disk compatibility scaffolding.** No reader for the old CA tree format, no dual-format code paths, no migration. Version fields in *new* formats are allowed; multi-version *handling* code is forbidden (per `feedback_ca_no_compat_scaffolding_predev`).

**Safety invariants that must never relax** (carried from `CaIncarnationCore.tla` + `CaBuildRootPrecommit.tla`)
- exact-token delete (`deleteExact`) is the only destructive authority; token mismatch is spared/replaced, never destructive;
- global registry fence precedes root-shard fences; fold-through-fence recheck precedes delete;
- `ViewableRound`: a round is writer-visible only after all its retired sets + part-manifest cleanup bundles are durable;
- `deadTok` / no-return: a deleted or overwritten token is never accepted as a future dependency;
- a writer that must resurrect a condemned blob re-uploads from its own source — **never** `GET`s the condemned object (per `feedback_ca_resurrect_invariant`);
- GC must never throw/fail-closed on a 404 during fold (record what you can and continue — per `feedback_ca_gc_never_throw_on_404`).

**Code style** (CI-enforced)
- Allman braces (opening brace on its own line).
- In prose/comments/commit messages: literal SQL keywords, class names, and function names in backticks (`MergeTree`); write a function as `f`, not `f()`; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- **Never use `sleep` in C++ to fix a race.**

**Build** (per CLAUDE.md)
- Build into a `build_*` directory (default `/home/mfilimonov/workspace/ClickHouse/master/build`). Always redirect ninja output to a log in the build dir. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `<build_dir>/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>[.sh]`. Do not add `no-*` tags unless strictly necessary. Prefer a new test over extending an existing one.
- Run CA gtests via `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*'`. The only tolerated baseline red is `CaWiringOps.FreezeViaHardLinksIntoShadow`.

**TLA+ run mechanics** (exact; from `docs/superpowers/models/`)
- Run one config via the Phase-0 wrapper:
  ```bash
  cd docs/superpowers/models
  ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_<cfg-basename>
  ```
  (For a long run set `TLC_JAVA_OPTS=-Xmx48g`.) The wrapper hardcodes `CaGcRootLocalPartManifestCore.tla` and writes `../../../tmp/tlc_<cfg>.log`.
- **HOLD config PASS** = exit 0 and the log contains `Model checking completed. No error has been found.`
- A **`_sab_*` / `_buggy`** config is correct **only when it FAILS** with `Error: Invariant <NAME> is violated.` (or `Temporal properties were violated.`). A zero exit on a `_sab_*` config is a **suite failure** (`UNEXPECTED PASS`).

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

- **Retire-token source completeness (spec §Round Protocol/Retire, §Phase Plan/Phase 5).** Phase 5 is allowed only after a model proves the stored token source is complete: every blob retired with `blobIndeg = 0` has a stored token, and that stored token equals the token that was current when the last in-degree edge was sealed. A stale stored token can only fail the exact match (under-delete), never match a re-incarnated object (over-delete). This is the *only* relaxation; everything else (fence, recheck, `ViewableRound`, exact-token delete) is unchanged.
- This phase does **not** touch part-manifest retire token capture (Phase 1d already lets manifest retire reuse the fold-captured token; that path is structural single-owner cleanup, not content delete). Phase 5 is strictly about the **blob** candidate `HEAD` at `Core/CasGc.cpp:1007`.

## Gate & Dependencies {#gate-dependencies}

- **Gate:** Phase 5 model extension GREEN — Task 1: `EnableRetireTokenSource` stage HOLDs on `INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_NO_RETURN`, and the negative control `SabotageStaleTokenOverDelete` VIOLATES `INV_NO_RETURN`; the whole `CaGcRootLocalPartManifestCore` suite re-greens.
- **Depends on:** Phase 4 (`2026-06-26-cas-gc-phase4-target-sharded-reducers.md`). The sealed generation/in-degree state from which retire sources the token is the Phase-1d–Phase-4 product; Phase 5 only adds a read of it.
- **This phase is OPTIONAL.** Implement Tasks 2-4 only if Task 1's gate passes. If Task 1 cannot be made green, stop and surface.

## File Structure {#file-structure}

- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` — add `storedTok` variable, `EnableRetireTokenSource` flag, `SabotageStaleTokenOverDelete` flag, the flag-gated branches in `GRetire`/`GRecheckDelete`/`Land`, and the `RetireTokenSourceComplete` invariant. (Task 1)
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5.cfg` — positive stage (must HOLD). (Task 1)
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_staletokenoverdelete.cfg` — negative control (must VIOLATE `INV_NO_RETURN`). (Task 1)
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md` — append the two Phase-5 rows. (Task 1)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h` / `.cpp` — store the observed token source in sealed generation state (the `RetiredEntry.token` provenance). (Task 2)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — drop the per-candidate `backend.head` at `:1007`; source the token from sealed state; `deleteExact` at `:382` unchanged. (Task 3)
- Modify: `src/Disks/tests/gtest_cas_gc_formats.cpp` — round-trip gtest for the stored-token field. (Task 2)
- Modify: `src/Disks/tests/gtest_cas_gc_round.cpp` — `CasGcRetire` gtest: under-delete on stale token, never over-delete, re-incarnated object spared. (Task 3)

## Canonical Contract (consumed; do not rename) {#canonical-contract-consumed-do-not-rename}

`CasBlobInDegree` zero-in-degree candidates; `RetiredEntry.token` (the exact incarnation token; `RetiredEntry{kind,hash,token,size}` in `CasGcFormats.h`); exact-token `deleteExact` (single site at `Core/CasGc.cpp:382`). Phase 5 lets `retire` source `RetiredEntry.token` from sealed generation state instead of a fresh `HEAD` at `Core/CasGc.cpp:1007`, preserving exact-token delete.

---

## Tasks {#tasks}

### Task 1 (GATE): Model extension — `EnableRetireTokenSource` stage + `SabotageStaleTokenOverDelete` control {#task-1-gate-model-extension-enableretiretokensource-stage-sabotagestaletokenoverdelete-control}

**Files:**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5.cfg`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_staletokenoverdelete.cfg`
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md`

**Interfaces produced:** `storedTok` variable; `EnableRetireTokenSource`, `SabotageStaleTokenOverDelete` constants; `RetireTokenSourceComplete` invariant; flag-gated `GRetire`/`GRecheckDelete`/`Land` branches.

> Model idiom reference: study `CaIncarnationCore.tla` `GRetire` (`:440`, `retired' = retired \cup {[h |-> h, t |-> tokOf[h], r |-> roundOf[l]]}`), `Land` (`:530`, exact-token: `present[d.h] /\ (SabotageUncondDelete \/ tokOf[d.h] = d.t)` then `deadTok' = ... \cup {tokOf[d.h]}`), `CondemnedAtView`/`RetiredHit` (`:120`), and the `SabotageReusedTag`/`SabotageUncondDelete` precedent (`:26`, `:227`, `:534`). Phase 5 reuses these exact shapes; the only new idea is *where the token in the retired entry comes from*.

- [ ] **Step 1: Add the `storedTok` variable and the two constants to the module.** In `CaGcRootLocalPartManifestCore.tla`, add `storedTok` to `CONSTANTS`' companion `VARIABLES` list and to `vars`, add the two flags to `CONSTANTS`, and initialize `storedTok` in `Init`.

  In the `CONSTANTS` block (next to the other `Enable*`/`Sabotage*` flags):
  ```tla
    EnableRetireTokenSource,        \* stage5: retire sources the token from sealed state, not a fresh HEAD
    SabotageStaleTokenOverDelete    \* negative control: accept a stale stored token as a non-exact delete authority
  ```
  In `VARIABLES`, add after `deadTok` (so the blob-object group is contiguous):
  ```tla
    storedTok,                      \* [Blobs -> 0..MaxToken] token captured into sealed gen state when the last
                                    \* in-degree edge was sealed; 0 = "no stored token". RetiredEntry.token source.
  ```
  Add `storedTok` to the `vars == << ... >>` tuple.
  In `Init`, add:
  ```tla
    /\ storedTok = [b \in Blobs |-> 0]
  ```
  Extend `TypeOK` with:
  ```tla
    /\ storedTok \in [Blobs -> 0..MaxToken]
  ```

- [ ] **Step 2: Seal the observed token when a blob in-degree edge is sealed.** In the action that emits a `+1` blob-edge delta into the sealed generation (the `GFoldTransition` `new`-owner branch added in Phase-0 Task 4), record the *current* token of each newly-edged present blob into `storedTok`. This models "the token observed and stored when the edge was sealed". Add to that branch's update (alongside the `blobEdges`/`blobIndeg` increments):
  ```tla
    /\ storedTok' = [b \in Blobs |->
                        IF b \in <the blobset being +1'd> /\ present[b] THEN tokOf[b] ELSE storedTok[b]]
  ```
  In every OTHER action's `UNCHANGED << ... >>`, add `storedTok` (it changes only in the seal branch). This is mechanical: TLC errors loudly (`storedTok is not assigned`) on any action that forgets it — fix each until `stage4` re-greens (Step 6).

  > Why current-token-at-seal is the right model of the implementation: in the code, the in-degree generation is built by folding manifest entries; the token that ends up in `RetiredEntry.token` under Phase 5 is the one captured when that blob's last surviving edge was sealed. A later in-place overwrite (`WMutableUpdate` does NOT touch blobs; the blob path is `WUploadBlob`/resurrection) advances `tokOf[b]` and joins the old token to `deadTok[b]`, which is exactly the staleness the proof must tolerate.

- [ ] **Step 3: Add the flag-gated retire-token source to `GRetire`.** Modify the blob `GRetire` action so the retired entry's token comes from `storedTok` when `EnableRetireTokenSource`, else from the freshly-observed `tokOf[b]` (the modeled per-candidate `HEAD`). Keep the existing zero-in-degree + present guard.
  ```tla
  \* Phase 5: retire-token source. Baseline (flag FALSE) = observe the CURRENT token now (the per-candidate
  \* HEAD). EnableRetireTokenSource = take the token sealed into generation state. SabotageStaleTokenOverDelete
  \* additionally lets the DELETE accept a stale stored token (see GRecheckDelete/Land), the over-delete hazard.
  RetireTokOf(b) == IF EnableRetireTokenSource THEN storedTok[b] ELSE tokOf[b]

  GRetireBlob(b) ==
      /\ blobIndeg[b] = 0 /\ present[b]
      /\ ~\E e \in retired : e.h = b /\ e.t = RetireTokOf(b)
      /\ retired' = retired \cup { [h |-> b, t |-> RetireTokOf(b), r |-> gcRound] }
      /\ UNCHANGED << present, tokOf, nextTok, deadTok, storedTok, (* ...all other vars... *) >>
  ```
  (Use the exact `UNCHANGED` list already present in the Phase-0 `GRetireBlob`, plus `storedTok`.)

- [ ] **Step 4: Keep the delete EXACT, and make the sabotage the only non-exact path.** The delete (`Land` / `GRecheckDelete`) must still match `tokOf[b]` exactly. `SabotageStaleTokenOverDelete` is the single flag that lets the delete fire on a stored token that is no longer current — i.e. it treats the *stored* value as authority rather than re-confirming exactness against the live `tokOf`. In `Land` (the exact-token landing), change the destructive guard so that the SABOTAGE path deletes when the retired entry's token merely equals the *stored* token (which may be stale), while the safe path keeps `tokOf[d.h] = d.t`:
  ```tla
  \* Exact-token landing. Safe: delete only if the live token still equals the carried token.
  \* SabotageStaleTokenOverDelete: delete if the carried token equals the STORED token even when the live
  \* token has moved on (a re-incarnation) — this is the over-delete the proof must reject (INV_NO_RETURN).
  Land(d) ==
      /\ ...
      /\ retired' = { e \in retired : ~(e.h = d.h /\ e.t = d.t) }
      /\ IF present[d.h]
            /\ ( (tokOf[d.h] = d.t)
                 \/ (SabotageStaleTokenOverDelete /\ d.t = storedTok[d.h]) )
         THEN /\ present'  = [present  EXCEPT ![d.h] = FALSE]
              /\ deadTok'   = [deadTok  EXCEPT ![d.h] = @ \cup {tokOf[d.h]}]
              /\ ...
         ELSE /\ UNCHANGED << present, deadTok >>
              /\ ...
      /\ UNCHANGED << ..., storedTok >>
  ```
  Mirror the same guard wherever `GRecheckDelete` issues the delete message, so the sabotage authority is consistent. **Do not** weaken the safe path: with `SabotageStaleTokenOverDelete = FALSE`, the only destructive condition is `tokOf[d.h] = d.t`.

- [ ] **Step 5: Add the `RetireTokenSourceComplete` invariant.** It states the proof obligation directly: under the stored-token source, every retired blob entry's token either equals the live token (exact, will delete) or is already dead (stale, will be spared by the exact match) — it can never be a *different currently-live* token (which would be the over-delete).
  ```tla
  RetireTokenSourceComplete ==
      EnableRetireTokenSource =>
          \A e \in retired :
              (e.t = tokOf[e.h]) \/ (e.t \in deadTok[e.h]) \/ (e.t = 0 /\ ~present[e.h])
  ```
  (The `e.t = 0 /\ ~present` disjunct covers a blob that never had a stored token and is absent — no candidate, no delete.)

- [ ] **Step 6: Re-green the existing suite (regression).** The new `storedTok` assignments must not break any earlier stage. Run the Phase-0 stages and the negative controls that touch the blob path; they must all still produce their expected result:
  ```bash
  cd docs/superpowers/models
  for s in stage0 stage1 stage2 stage3 stage4 live ; do ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_$s ; done
  ```
  Expected: every one prints `Model checking completed. No error has been found.` and `exit=0`. If `storedTok is not assigned` appears, add `storedTok` to the offending action's `UNCHANGED`; if a stage finds a real trace, the seal-time assignment in Step 2 is wrong — fix it, since all flags are still off here.

- [ ] **Step 7: Write `stage5.cfg`** — copy `CaGcRootLocalPartManifestCore_stage4.cfg`, then set `EnableRetireTokenSource = TRUE`, keep `SabotageStaleTokenOverDelete = FALSE` (and all other `Sabotage* = FALSE`), and add the new invariants. The constant block must list **every** `Sabotage*`/`Enable*`/`SabotageStaleTokenOverDelete` flag (TLC requires all constants assigned). Add to the invariant lines:
  ```
  CONSTANTS
      ...
      EnableRetireTokenSource = TRUE
      SabotageStaleTokenOverDelete = FALSE
  INVARIANT TypeOK
  INVARIANT INV_NO_DANGLE
  INVARIANT INV_NO_LOSS
  INVARIANT INV_NO_RETURN
  INVARIANT RetireTokenSourceComplete
  ```
  Keep `MaxToken >= 3` (need room for re-incarnation: original, condemned, fresh) and `Blobs = {b1, b2}` so a shared/re-uploaded blob is reachable.

- [ ] **Step 8: Run `stage5` — must HOLD.**

  Run: `TLC_JAVA_OPTS=-Xmx16g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage5`
  Expected: `Model checking completed. No error has been found.` and `exit=0`. If it finds a trace, the relaxation is unsafe as modeled — **stop and surface** (do not proceed to code); the gate has failed.

- [ ] **Step 9: Write `sab_staletokenoverdelete.cfg`** — copy `stage5.cfg`, then set `SabotageStaleTokenOverDelete = TRUE`, and narrow to the single targeted invariant (the `CaIncarnationCore` convention — narrowing speeds the counterexample):
  ```
  CONSTANTS
      ...
      EnableRetireTokenSource = TRUE
      SabotageStaleTokenOverDelete = TRUE
  INVARIANT INV_NO_RETURN
  ```

- [ ] **Step 10: Run the negative control — must FAIL with `INV_NO_RETURN`.**

  Run: `./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_staletokenoverdelete && echo "UNEXPECTED PASS"`
  Expected: `Error: Invariant INV_NO_RETURN is violated.`, a nonzero `exit=`, and **no** `UNEXPECTED PASS` line. (The counterexample: a blob is condemned with a stored token, the object is re-incarnated to a fresh token, and the sabotage path deletes it on the stale stored token — a deleted-then-resurrected token becomes current and is then destroyed, the no-return violation.) If it PASSES, the sabotage is not actually weakening the delete authority — fix the `Land`/`GRecheckDelete` guard in Step 4 until the counterexample appears.

- [ ] **Step 11: Append the two rows to `CaGcRootLocalPartManifestCore_RESULTS.md`** — one row for `stage5` (HOLD; record `states generated`, `distinct states`, wall time from the log) and one for `sab_staletokenoverdelete` (VIOLATED `INV_NO_RETURN`). Use the analyze-via-subagent rule for the logs.

- [ ] **Step 12: Commit.**
  ```bash
  git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla \
          docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5.cfg \
          docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_staletokenoverdelete.cfg \
          docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md
  git commit -m "CA GC phase5: retire-token model gate — stage5 HOLDs, stale-token over-delete violates INV_NO_RETURN

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
  ```

**GATE CHECK before Task 2:** confirm `stage5` HOLDs on `INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_NO_RETURN`/`RetireTokenSourceComplete`, `sab_staletokenoverdelete` VIOLATES `INV_NO_RETURN`, and the full Phase-0 suite still re-greens. If any of these fail, **stop**: Phase 5 is not safe to implement.

---

### Task 2: Store the observed token source in sealed generation state at fold/retire time + round-trip gtest {#task-2-store-the-observed-token-source-in-sealed-generation-state-at-fold-retire-time-round-trip-gtest}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp`
- Modify: `src/Disks/tests/gtest_cas_gc_formats.cpp`

**Interfaces produced:** a durable per-blob `stored_token` (an `ObjectToken`) carried in the sealed in-degree/generation state — the provenance for `RetiredEntry.token` under Phase 5. This is the `storedTok` of Task 1 made concrete.

- [ ] **Step 1 (failing test first — TDD):** add a round-trip gtest that encodes sealed generation state carrying a per-blob stored token and decodes it byte-for-byte. In `src/Disks/tests/gtest_cas_gc_formats.cpp`, add:
  ```cpp
  TEST(CasGcFormats, StoredTokenRoundTripsInSealedState)
  {
      using namespace DB::Cas;
      /// Build the sealed-state record that Phase 5 reads the retire token from.
      /// <use the actual sealed-generation struct from CasGcFormats.h that Phase 1d added;
      ///  set a blob hash -> ObjectToken{value="etag-7", type=ETag} entry>
      const auto encoded = encode<SealedGenStateType>(state);
      const auto decoded = decode<SealedGenStateType>(encoded);
      EXPECT_EQ(decoded.storedToken(blobHash).value, "etag-7");
      /// Strictness: the stored token must survive re-encode identically (write-once, replay-safe).
      EXPECT_EQ(encode<SealedGenStateType>(decoded), encoded);
  }
  ```
  Replace `SealedGenStateType` / `encode`/`decode` / `storedToken` with the exact names the Phase-1d/Phase-4 generation-seal format uses (read `CasGcFormats.h` and `CasGenerationSeal.h` to confirm; this plan does not invent a new container — it extends the sealed-state record that already carries per-blob in-degree).

- [ ] **Step 2: Run it — must FAIL to compile or assert** (the field does not exist yet):
  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > test_p5_formats.log 2>&1 ; tail -3 test_p5_formats.log
  ```
  Expected: a compile error naming the missing `stored_token` / `storedToken` member (red — the test cannot yet pass). Analyze the log with a subagent.

- [ ] **Step 3: Add the field + codec.** In `CasGcFormats.h`, add a `stored_token` (`ObjectToken`/`Token`) to the per-blob sealed in-degree record (the same record that already carries the blob hash + in-degree), with a comment: `/// Phase 5: the exact incarnation token observed when this blob's last in-degree edge was sealed; retire sources RetiredEntry.token from here instead of a fresh HEAD (exact-token delete preserved).` In `CasGcFormats.cpp`, extend the encode/decode of that record to round-trip the token value + type, fail-closed on a malformed token exactly like the existing `RetiredEntry` codec (`decodeRetiredSet`).

- [ ] **Step 4: Re-run the gtest — must PASS.**
  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > test_p5_formats.log 2>&1 ; tail -3 test_p5_formats.log && ./src/unit_tests_dbms --gtest_filter='CasGcFormats.StoredTokenRoundTripsInSealedState' > test_p5_formats_run.log 2>&1 ; grep -E '\[  PASSED  \]|\[  FAILED  \]' test_p5_formats_run.log
  ```
  Expected: `[  PASSED  ] 1 test.`. Analyze both logs with a subagent.

- [ ] **Step 5: Commit.**
  ```bash
  git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h \
          src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp \
          src/Disks/tests/gtest_cas_gc_formats.cpp
  git commit -m "CA GC phase5: carry per-blob stored_token in sealed generation state + round-trip gtest

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
  ```

---

### Task 3: Drop the per-candidate `HEAD` in retire; delete uses the stored token (still exact) + `CasGcRetire` gtest {#task-3-drop-the-per-candidate-head-in-retire-delete-uses-the-stored-token-still-exact-casgcretire-gtest}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- Modify: `src/Disks/tests/gtest_cas_gc_round.cpp`

**Interfaces produced:** `Gc::retire` sources `RetiredEntry.token` from sealed generation state (the Task-2 field) instead of `backend.head` at `Core/CasGc.cpp:1007`; `deleteExact` at `Core/CasGc.cpp:382` unchanged.

- [ ] **Step 1 (failing test first — TDD):** add a `CasGcRetire` test to `src/Disks/tests/gtest_cas_gc_round.cpp` next to the existing `CasGcRetire` tests (e.g. after `ObservesCurrentTokenDeletesExactAndDropsEntries` at `:818`). It asserts the three proof corners from Task 1 against the real `Gc` over `CasInMemoryBackend`:
  ```cpp
  TEST(CasGcRetire, SourcesTokenFromSealedStateAndStaysExact)
  {
      /// Use the same harness as ObservesCurrentTokenDeletesExactAndDropsEntries.
      /// (1) A condemned blob whose STORED token is STALE: retire writes the stored token; the
      ///     exact-token delete 412s (TokenMismatch) -> Spared/Replaced. UNDER-delete: live bytes kept.
      /// (2) A re-incarnated object is NEVER over-deleted: after a fresh upload (new token), retire's
      ///     stored token no longer matches, so deleteExact does not remove the live bytes.
      /// (3) A blob whose stored token IS current is deleted exactly (the happy path).
      /// Assert: NO backend.head() is issued per candidate in retire (count HEADs via the in-memory
      ///         backend's op counter, or assert the candidate loop reads the sealed token).
      ...
  }
  ```
  Use `displaceObjectToken` (already imported at the top of the file) to re-incarnate a blob and force the stale-token path. Confirm the in-memory backend exposes a HEAD/op counter to assert "no per-candidate `HEAD`"; if it does not, assert the weaker observable — the retired entry's token equals the sealed stored token, not the live one — and note that in the test comment.

- [ ] **Step 2: Run it — must FAIL** (retire still issues the `HEAD` and uses the live token):
  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > test_p5_retire.log 2>&1 ; tail -3 test_p5_retire.log && ./src/unit_tests_dbms --gtest_filter='CasGcRetire.SourcesTokenFromSealedStateAndStaysExact' > test_p5_retire_run.log 2>&1 ; grep -E '\[  PASSED  \]|\[  FAILED  \]' test_p5_retire_run.log
  ```
  Expected: `[  FAILED  ]` (red). Analyze with a subagent.

- [ ] **Step 3: Change `retire` to source the token from sealed state.** In `Core/CasGc.cpp`, in `Gc::retire` (`:971`), replace the per-candidate `const HeadResult observed = backend.head(objectKey(...))` at `:1007` with a read of the Task-2 `stored_token` from the threaded sealed generation/snap state for `candidate.hash`. Set `entry.token` from the stored token instead of `observed.token` (`:1110`). Keep the absent-candidate handling: a candidate with **no** stored token (or an absent object) is skipped/forgotten exactly as the current `!observed.exists` branch does — do not fabricate a token. Update the `GcRetireObserve`/`GcRetireDecision`/`BlobRetire` event `reason` strings to say the token is sourced from sealed state, not a fresh `HEAD`. **Do not touch** `deleteExact` at `:382` — the recheck still deletes with `entry.token` exactly, and a stale stored token simply 412s (`TokenMismatch` -> `Replaced`), which is the modeled under-delete.

  > Keep the comment block at `:990`-`:1009` honest: it currently says "One HEAD per candidate captures the CURRENT incarnation token". Rewrite it to: the token is now read from the sealed generation state (Phase 5, gated by the green `EnableRetireTokenSource` model); a stale stored token can only fail the exact-token match (Spared/Replaced), never match a re-incarnated object — proved by `CaGcRootLocalPartManifestCore` `stage5` + `sab_staletokenoverdelete`.

- [ ] **Step 4: Re-run the gtest — must PASS.**
  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > test_p5_retire.log 2>&1 ; tail -3 test_p5_retire.log && ./src/unit_tests_dbms --gtest_filter='CasGcRetire.*' > test_p5_retire_run.log 2>&1 ; grep -E '\[  PASSED  \]|\[  FAILED  \]' test_p5_retire_run.log | tail -3
  ```
  Expected: `[  PASSED  ]` for the new test and all sibling `CasGcRetire.*` tests (the existing `ObservesCurrentTokenDeletesExactAndDropsEntries` may need its expectation updated if it asserted a per-candidate `HEAD`; if so, update it to assert the sealed-token source — that is a legitimate consequence of this phase, not a regression). Analyze with a subagent.

- [ ] **Step 5: Commit.**
  ```bash
  git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
          src/Disks/tests/gtest_cas_gc_round.cpp
  git commit -m "CA GC phase5: retire sources RetiredEntry.token from sealed state, drops per-candidate HEAD (delete stays exact)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
  ```

---

### Task 4: Build + full `Cas*`/`Ca*` gtest sweep + commit {#task-4-build-full-cas-ca-gtest-sweep-commit}

**Files:** none new — verification gate.

- [ ] **Step 1: Clean build + full sweep.** Redirect to logs in the build dir; analyze each log with a subagent (per CLAUDE.md — never paste raw build output).
  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > test_p5_sweep_build.log 2>&1 ; tail -5 test_p5_sweep_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > test_p5_sweep.log 2>&1 ; grep -E '\[  PASSED  \]|\[  FAILED  \]' test_p5_sweep.log | tail -8
  ```
  Expected: the build log ends with a `[N/N]` link line and no `error:`; the sweep shows all `Cas*`/`Ca*` green with the **only** tolerated red being the baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`. Have a subagent return: total passed/failed, the names of any unexpected failures, and the last build-log lines.

- [ ] **Step 2: If anything other than the baseline red fails,** triage with superpowers:systematic-debugging — Phase 5 changes only the retire token source, so any new failure is either an updated expectation (legitimately fix the test to assert the sealed-token source) or a real bug in Task 3 (fix the code). Do not weaken `deleteExact`. Re-run Step 1 until green.

- [ ] **Step 3: Commit** (only if Steps 1-3 made any fixes; otherwise the phase ends at Task 3's commit).
  ```bash
  git add -A
  git commit -m "CA GC phase5: full Cas*/Ca* gtest sweep green after retire-token optimization

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
  ```

---

## Self-Review {#self-review}

- **Optional + gated, stated up front.** The header, the goal, and the Gate section all say Phase 5 is OPTIONAL and that Tasks 2-4 run only if Task 1's model gate passes; Task 1 ends with an explicit GATE CHECK that says "stop" on failure. ✓
- **Matches the spec's Phase-5 rule** (§Phase Plan/Phase 5, §Round Protocol/Retire): the per-candidate `HEAD` is removed only after a model proves the stored token source is complete and stale tokens cause UNDER-delete, never OVER-delete. The `RetireTokenSourceComplete` invariant + `SabotageStaleTokenOverDelete` ⇒ `INV_NO_RETURN` encode exactly that. ✓
- **Canonical contract consumed, not renamed.** `CasBlobInDegree` zero-in-degree candidates, `RetiredEntry.token` (`{kind,hash,token,size}`), exact-token `deleteExact`. The only changed thing is the token *source* (`Core/CasGc.cpp:1007`); `deleteExact` (`:382`) is explicitly untouched. ✓
- **TDD, bite-sized, real commands + expected output, frequent commits.** Each code task writes a failing test, runs it (red), implements, re-runs (green), commits — with concrete `ninja`/`unit_tests_dbms` commands, the actual gtest binary, real file paths, and a stated expected result. The model task runs the wrapper with named cfgs and stated PASS/VIOLATE expectations. ✓
- **Model idiom is grounded, not invented.** Task 1 cites the exact `CaIncarnationCore.tla` lines (`GRetire` `:440`, `Land` `:530`, `CondemnedAtView` `:120`, `SabotageReusedTag`/`SabotageUncondDelete`) the implementer mirrors, and extends the Phase-0 module + wrapper rather than starting fresh. The two new cfgs are fully specified (base stage + the single flag + the targeted invariant), which is data, not a vague instruction. ✓
- **Allman braces; literal names in backticks; "exception" not "crash"; no `sleep`.** Prose and the C++ test sketch follow the style rules. ✓
- **Honest unknowns flagged, not papered over.** Task 2 tells the implementer to read `CasGcFormats.h`/`CasGenerationSeal.h` for the exact sealed-state container name rather than inventing one; Task 3 gives a fallback assertion if the in-memory backend lacks a HEAD/op counter. These are real ground-truth lookups the Phase-1d/Phase-4 code will have fixed by the time Phase 5 runs. ✓
