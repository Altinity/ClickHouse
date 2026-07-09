# CAS promote tokenless copy-forward condemn-race — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `ALTER TABLE ... DETACH PARTITION` (and other tokenless `adoptEvidence` commit paths) succeed when GC prematurely condemns an adopted blob whose condemnation is only visible after the in-closure fence refresh — by adding an in-closure copy-forward backstop to `Build::promote`'s revalidation loop.

**Architecture:** `Build::promote` already copy-forwards condemned tokenless blobs in a pre-pass, but that pre-pass runs against the pre-refresh retire view; the in-closure revalidation checks the post-refresh view and aborts on any condemnation the pre-pass missed. This plan adds an in-closure copy-forward for tokenless copy-forwardable leaves (after the refresh + owner-liveness check), factors the classification into one shared predicate, and keeps the pre-pass as the outside-the-lock fast path.

**Tech Stack:** C++ (ClickHouse CAS core), GoogleTest (`build/src/unit_tests_dbms`), TLA+/TLC (already gated).

## Global Constraints

- Branch `cas-gc-rebuild`. New commits only — never rebase/amend.
- Allman braces; no `sleep`-based synchronization.
- Runtime errors use `ErrorCodes::ABORTED`, never `LOGICAL_ERROR`.
- INV-1: the tokenless copy-forward (`copyForwardFromCondemned`) is the ONLY sanctioned read of a condemned-but-present object, valid ONLY for tokenless `adoptEvidence` deps (independent committed owner). It re-verifies the payload hash before republishing. Tokened leaves resurrect via `uploadFromSource` (no GET); unknown leaves (no dep) and absent-no-source leaves fail closed (`ABORTED`).
- Copy-forward/resurrect runs ONLY after the owner-liveness check (no consequential action on an aborting path).
- Build binary into `build/` FOREGROUND: `ninja` blocking, redirect to `build/*.log`, NO `-j`/`nproc`. Unit binary `build/src/unit_tests_dbms`. Use a subagent to summarize any build log.
- Commit trailers: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`.

---

### Task 1: TLA+ gate — confirm evidence re-observe (WEvObserve) is modelled and green

**Files:** verify only — `docs/superpowers/models/CaIncarnationCore.tla`, `..._stage6_evstale.cfg`, `..._sab_noevreobserve.cfg`, `run_tlc.sh`.

**Interfaces:** Produces the mapping — the in-closure copy-forward backstop against the refreshed view ≙ the model's `WEvObserve` (re-observe stale tokenless evidence before the publish gate). `stage6_evstale` PASS + `sab_noevreobserve` MUST dangle authorize Tasks 2–4.

- [ ] **Step 1: Run the positive gate**

```bash
cd docs/superpowers/models && TLC_JAVA_OPTS="-Xmx8g" ./run_tlc.sh CaIncarnationCore_stage6_evstale.cfg
```
Expected: `Model checking completed. No error has been found.`, `exit=0` (EnableEvStale=TRUE, EnableReval=TRUE; INV_NO_DANGLE/INV_NO_LOSS/INV_NO_RETURN hold).

- [ ] **Step 2: Run the negative control**

```bash
cd docs/superpowers/models && TLC_JAVA_OPTS="-Xmx8g" ./run_tlc.sh CaIncarnationCore_sab_noevreobserve.cfg
```
Expected: an invariant violation (`INV_NO_DANGLE` or `INV_NO_LOSS`) — admitting stale evidence without re-observation MUST dangle, confirming the re-observe (our copy-forward backstop) is load-bearing.

- [ ] **Step 3: Record the mapping + commit**

Append the two TLC results + the mapping (in-closure copy-forward ≙ `WEvObserve`) to `docs/superpowers/worklogs/2026-07-08-unattended-cas-cache-and-stabilization.md`; commit (`docs(cas): TLA+ gate — stage6_evstale green + sab_noevreobserve dangles authorize tokenless copy-forward backstop`).

---

### Task 2: Factor the copy-forwardable-tokenless predicate into one helper

**Files:**
- Modify: `Core/CasBuild.h` (decl), `Core/CasBuild.cpp` (def + reroute the pre-pass)

**Interfaces:**
- Produces: `bool Build::isCopyForwardableTokenless(const UInt128 & hash) const;` — true iff this build holds a Blob dep for `hash` with `token == std::nullopt` (a tokenless W-EVIDENCE / `adoptEvidence` dep). Consumed by the pre-pass (Task 2) and the backstop (Task 3).

- [ ] **Step 1: Add the helper declaration + definition**

`CasBuild.h`, private section near `retainedSourceFor`:
```cpp
    /// A leaf is copy-forwardable iff this build holds a TOKENLESS W-EVIDENCE Blob dep for `hash`
    /// (adoptEvidence — an independent live committed owner exists, so the INV-1 copy-forward exception
    /// applies). A tokened dep, or NO dep at all (a staging bug — must fail closed), is NOT
    /// copy-forwardable. Single source of truth for both the promote pre-pass and the in-closure backstop.
    bool isCopyForwardableTokenless(const UInt128 & hash) const;
```
`CasBuild.cpp` (near `retainedSourceFor`):
```cpp
bool Build::isCopyForwardableTokenless(const UInt128 & hash) const
{
    auto it = deps.find({static_cast<uint8_t>(ObjectKind::Blob), hash});
    return it != deps.end() && !it->second.token.has_value();
}
```

- [ ] **Step 2: Reroute the pre-pass through the helper**

In `Build::promote` (`CasBuild.cpp:816–829`), replace the inline classification:
```cpp
        const auto dep = deps.find({static_cast<uint8_t>(ObjectKind::Blob), e.blob_hash});
        if (dep == deps.end() || dep->second.token.has_value())
            continue;
```
with:
```cpp
        if (!isCopyForwardableTokenless(e.blob_hash))
            continue;
```

- [ ] **Step 3: Build + run the CA gtests (no behavior change — pure refactor)**

```bash
cd build && ninja unit_tests_dbms > build_cf_task2.log 2>&1
./src/unit_tests_dbms --gtest_filter='Ca*:*Cas*' > ../tmp/cf_gtests_task2.log 2>&1; tail -4 ../tmp/cf_gtests_task2.log
```
Expected: same pass/fail set as before this task (the 3 known-flaky `CaWiring*` remain; nothing new).

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp
git commit -m "refactor(cas): factor isCopyForwardableTokenless predicate (promote pre-pass)"
```

---

### Task 3: In-closure copy-forward backstop in the revalidation loop

**Files:**
- Modify: `Core/CasBuild.cpp` (`Build::promote` revalidation loop — the condemned branch; + the pre-pass comment)

**Interfaces:**
- Consumes: `isCopyForwardableTokenless` (Task 2); `copyForwardFromCondemned(hash, key, hr)` (`CasBuild.cpp:506`); the loop's `hr` (`HeadResult`).
- Produces: promote that copy-forwards a tokenless present-condemned leaf in-closure (against the refreshed view) instead of aborting; absent-tokenless and unknown-leaf still abort.

- [ ] **Step 1: Write the failing test** — Task 4's `PromoteCopiesForwardCondemnedEvidenceRevealedAfterRefresh`; confirm it FAILS (ABORTED) against current code before this task.

- [ ] **Step 2: Verify current promote fails the test**

```bash
cd build && ninja unit_tests_dbms > build_cf_task3.log 2>&1
./src/unit_tests_dbms --gtest_filter='*PromoteCopiesForwardCondemnedEvidenceRevealedAfterRefresh*'
```
Expected: FAIL — `ABORTED` "condemned at commit revalidation".

- [ ] **Step 3: Add the in-closure copy-forward to the condemned branch**

In the revalidation loop's `if (store->retireView().isCondemnedToken(...))` block, the current shape is:
```cpp
                if (store->retireView().isCondemnedToken(ObjectKind::Blob, e.blob_hash, hr.token))
                {
                    if (!src)
                        throw Exception(ErrorCodes::ABORTED,
                            "promote: blob {} condemned at commit revalidation — failing closed (INV-1)", blob_key);
                    uploadFromSource(ObjectKind::Blob, e.blob_hash, blob_key, *src);
                    continue;
                }
```
Replace with:
```cpp
                if (store->retireView().isCondemnedToken(ObjectKind::Blob, e.blob_hash, hr.token))
                {
                    /// Resurrect-on-condemn against the POST-refresh view (the pre-pass ran against the
                    /// pre-refresh view and may have missed this condemnation). Tokened ⇒ re-upload from the
                    /// retained source (INV-1, no GET). Tokenless copy-forwardable (adoptEvidence, independent
                    /// committed owner) ⇒ verified copy-forward of the condemned-but-present incarnation (the
                    /// documented INV-1 exception). Safe here ONLY because we are past the owner-liveness check
                    /// above (this build's precommit is the live owner) and the promote fold barrier guarantees
                    /// the detached precommit's +edge folds before this promote — so the leaf is legitimately
                    /// protected. Unknown leaf (no dep) or tokened-source-lost ⇒ fail closed.
                    if (src)
                        uploadFromSource(ObjectKind::Blob, e.blob_hash, blob_key, *src);
                    else if (isCopyForwardableTokenless(e.blob_hash))
                        copyForwardFromCondemned(e.blob_hash, blob_key, hr);
                    else
                        throw Exception(ErrorCodes::ABORTED,
                            "promote: blob {} condemned at commit revalidation — failing closed (INV-1)", blob_key);
                    continue;
                }
```
(Leave the ABSENT branch unchanged: `!hr.exists` with no `src` stays `ABORTED` — `copyForwardFromCondemned` cannot recreate an absent object.) Update the pre-pass comment at `CasBuild.cpp:814–815` — the in-closure gate is no longer a brick for tokenless leaves; it now copy-forwards a refresh-revealed condemnation (the pre-pass remains the outside-the-lock fast path).

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd build && ninja unit_tests_dbms > build_cf_task3b.log 2>&1
./src/unit_tests_dbms --gtest_filter='*PromoteCopiesForwardCondemnedEvidenceRevealedAfterRefresh*'
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp
git commit -m "fix(cas): promote copies forward a refresh-revealed condemned tokenless blob in-closure (INV-1)"
```

---

### Task 4: gtests — copy-forward succeeds; absent + unknown-leaf still abort

**Files:** Test: `src/Disks/tests/gtest_ca_wiring.cpp` (use the fixture that builds a `Store`, adopts a blob via `adoptEvidence`, and can seed a retired set + fence the namespace — mirror `gtest_cas_protocol_scenarios.cpp` `FenceConflict…`/`RevalidateAbsent…` and the existing `CaWiringExchange.AdoptFailsClosedAndFallsBackOnCondemnedBlob` at gtest_ca_wiring.cpp:1155).

**Interfaces:** Consumes the existing wiring/adopt fixtures + the retire-view seed (`seedRetired`/`injectRetire` + `fenceNamespace`) that installs a condemnation at a round ahead of the current view (so it is only visible after the in-closure refresh).

- [ ] **Step 1: Test A — copy-forward succeeds (RED driver for Task 3)**

```cpp
// A tokenless (adoptEvidence) blob condemned at a round ahead of the writer's view — visible only after
// the in-closure fence refresh — is copied forward in-closure; promote SUCCEEDS.
TEST(<fixture>, PromoteCopiesForwardCondemnedEvidenceRevealedAfterRefresh)
{
    // 1. Publish a committed source part with blob X (so X has an independent live committed owner).
    // 2. Stage a second build that ADOPTS X via adoptEvidence (tokenless dep, no putBlob/source) +
    //    stageManifest + precommitAdd (do NOT promote yet).
    // 3. Seed a retired set condemning X's current token AND fence the namespace at round R+1 so the
    //    condemnation is ahead of the writer's current view (mirrors FenceConflict… setup) — the pre-pass
    //    (pre-refresh view) will NOT see it; the in-closure refresh will.
    // 4. EXPECT_NO_THROW(build->promote(...)) — X is copied forward to a fresh live incarnation.
    // 5. Assert: the ref resolves AND X rides a fresh, non-condemned token.
}
```

- [ ] **Step 2: Test B — absent tokenless still aborts**

```cpp
// A tokenless adopted blob that is ABSENT (deleted, no source) at promote still fails closed.
TEST(<fixture>, PromoteAbsentTokenlessBlobAbortsRetryable)
{
    // adoptEvidence X; delete X (deleteExact); promote → EXPECT_THROW ABORTED "absent …"; no ref published.
}
```

- [ ] **Step 3: Test C — unknown leaf (no dep) condemned still aborts**

```cpp
// A manifest blob leaf with NO recorded dep (a staging-bug shape) that is condemned must fail closed —
// isCopyForwardableTokenless is false, so no silent copy-forward.
TEST(<fixture>, PromoteCondemnedLeafWithoutDepAbortsFailClosed)
{
    // Construct a build whose staged manifest references a blob hash for which deps has NO entry;
    // condemn it; promote → EXPECT_THROW ABORTED. (If this shape is not reachable through the public
    // build API, document why and rely on the isCopyForwardableTokenless unit assertion instead.)
}
```

- [ ] **Step 4: Run all three + regression sweep**

```bash
cd build && ninja unit_tests_dbms > build_cf_task4.log 2>&1
./src/unit_tests_dbms --gtest_filter='*PromoteCopiesForwardCondemnedEvidenceRevealedAfterRefresh*:*PromoteAbsentTokenlessBlobAbortsRetryable*:*PromoteCondemnedLeafWithoutDepAbortsFailClosed*'
./src/unit_tests_dbms --gtest_filter='Ca*:*Cas*' > ../tmp/cf_gtests_after.log 2>&1; tail -5 ../tmp/cf_gtests_after.log
```
Expected: the three pass; the `Ca*/*Cas*` failure set unchanged (3 known-flaky `CaWiring*` only, not grown).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "test(cas): promote copy-forwards refresh-revealed condemned evidence; absent/no-dep still abort"
```

---

## Self-Review

**Spec coverage:** in-closure backstop → Task 3; shared predicate → Task 2; unknown/absent fail-closed → Task 3 + Task 4 B/C; keep pre-pass → Task 2 (reroute, not remove); TLA+ gate → Task 1; invariant comment → Task 3 Step 3. The optional pre-pass refresh (spec §3) is deferred (fast-path-only, not required for correctness) — noted, not a task. Stateless `03283` validation is post-merge (needs the S3 lane), not a task.

**Placeholder scan:** Tasks 2–3 carry complete literal C++. Task 4 test bodies are commented outlines (the retire-seed/fence helpers depend on the chosen fixture the implementer reads first); assertions + expected outcomes are explicit. Test C notes a fallback if the no-dep shape is unreachable through the public API.

**Type consistency:** `isCopyForwardableTokenless(const UInt128&) -> bool`, `copyForwardFromCondemned(const UInt128&, const String&, HeadResult) -> Token`, `uploadFromSource(ObjectKind, const UInt128&, const String&, const BlobSource&)` used consistently across Tasks 2–3.

## Execution Handoff

Plan complete and saved. Execution: **Subagent-Driven** — small but delicate (commit protocol); Tasks 2–4 are one coherent change (helper → backstop → tests) suitable for a single implementer unit with a task review, then a whole-branch review.
