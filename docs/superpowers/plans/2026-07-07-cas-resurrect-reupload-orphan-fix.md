# RESURRECT-REUPLOAD-ORPHAN fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the content-addressed GC from orphaning a resurrect-replaced blob incarnation, by making the fold's condemn decision key on `(hash, current token)` instead of on the hash.

**Architecture:** In `foldDeltasIntoGeneration`'s per-blob close-out (`closeBlob`, `CasBlobInDegree.cpp`), when a hash is touched this fold window with net in-degree 0 AND a prior retired entry exists whose token differs from the current object's token (a resurrect replaced it), supersede the stale entry with a fresh condemn of the current token. The new entry rides the existing R5 round CAS; a new `RetiredMergeResult.replaced` field carries these so the caller emits a `blob_retire_replaced` CA-log event and bumps `CasGcRetireReplaced`.

**Tech Stack:** C++ (ClickHouse), GoogleTest (`unit_tests_dbms`), TLA+/TLC (`tmp/tla2tools.jar`).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-07-cas-resurrect-reupload-orphan-fix-design.md`.
- Condemn keys on `(hash, current token)`; the HEAD fires ONLY in `closeBlob` (touched hashes) — quiescent entries via `settleRetiredBelow` take no HEAD (do not change that path).
- Safety: re-condemn only when `cur_edges == 0` (unreferenced) — never condemn a hash with `cur_edges > 0` (that stays the existing `spared`/recovery branch).
- One retired entry per hash: on supersede, DROP the stale entry (do not keep two entries for one hash); the retired list stays sorted by hash (entries are produced in-order inside the merge).
- Allman braces; do not add `no-*` test tags; C++ style-check clean.
- Build into `build/` (NOT `build_asan`); never pass `-j`/`nproc` to ninja; redirect ninja output to a log in `build/` and have a subagent summarize it.
- Commit trailers: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`. Commit on the current branch (`cas-gc-rebuild`), never master.

---

### Task 0: TLA+ gate — align the focused model to the final rule

**Files:**
- Modify: `docs/superpowers/models/CaGcResurrectReuploadOrphan.tla`
- Use: `docs/superpowers/models/CaGcResurrectReuploadOrphan_bug.cfg`, `_fix.cfg`

**Interfaces:**
- Produces: a validated model where `_bug.cfg` violates `NoLeakForever` and `_fix.cfg` holds, with the fix branch expressed as "condemn the current token on `touched ∧ in-degree==0`, keyed on the current token".

- [ ] **Step 1: Confirm the current model still splits bug/fix.** Run both cfgs:

```bash
cd docs/superpowers/models
java -cp ../../../tmp/tla2tools.jar tlc2.TLC -deadlock -config CaGcResurrectReuploadOrphan_bug.cfg CaGcResurrectReuploadOrphan.tla 2>&1 | grep -aE "violated|No error|Finished"
java -cp ../../../tmp/tla2tools.jar tlc2.TLC -deadlock -config CaGcResurrectReuploadOrphan_fix.cfg CaGcResurrectReuploadOrphan.tla 2>&1 | grep -aE "violated|No error|Finished"
```
Expected: bug → "Temporal properties were violated"; fix → "No error has been found".

- [ ] **Step 2: Align the fix branch to key on the current token.** In `GcFold`, the fix branch (`FixReCondemnCurrentToken`) already re-condemns the current token when settling a prior entry whose token differs at `cur_edges==0`. Ensure the comment/guard reads as the final rule: re-condemn keyed on the current token, gated on `touched && in-degree==0`. If already so, no code change — just verify the comment matches the spec's §Fix wording.

- [ ] **Step 3: Re-run both cfgs (Step 1) and confirm the same bug/fix split.** Expected unchanged: bug violates, fix holds.

- [ ] **Step 4: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaGcResurrectReuploadOrphan.tla
git commit -m "tla: align resurrect-reupload-orphan fix branch to (hash,current-token) re-condemn rule"
```

---

### Task 1: RED unit test — resurrect-replaced incarnation must be reclaimed

**Files:**
- Modify: `src/Disks/tests/gtest_cas_gc_leak.cpp` (add one `TEST`)
- Read: `src/Disks/tests/cas_test_helpers.h` (helpers), the existing `TEST(CasGcLeak, DroppedPartFullyReclaimed)` (template)

**Interfaces:**
- Consumes (existing): `publishOneBlobPart(s, ns, ref, payload) -> ManifestId`; `s->startBuild(BuildInfo) -> BuildPtr`; `build->putBlob(idOf(p), BlobSource::fromString(p))`; `build->stageManifest({blobEntry(...)}) -> ManifestId`; `build->precommitAdd(ns, ref, id)`; `build->promote(ns, ref, build->buildId(), id)`; `s->dropRef(ns, ref)`; `gc.runRegularRound() -> RoundReport`; `runGcToFixpoint(s, gc)`; `runFsck(*s, false) -> FsckReport{reachable,dangling,unreachable}`; `blobPresent(b, layout, payload) -> bool`; `inDegreeOf(*b, layout, u128Of(payload)) -> int`; `s->retireView().refresh()`; `s->retireView().isCondemnedToken(ObjectKind::Blob, hash, token)`; `b->head(layout.blobKey(BlobId(...)))`.
- Produces: `TEST(CasGcLeak, ResurrectReplacedIncarnationReclaimed)`.

- [ ] **Step 1: Write the failing test.** Add to `gtest_cas_gc_leak.cpp` (after `DroppedPartFullyReclaimed`). The choreography: reference+drop payload `P` (condemn token A); drive ONE round to condemn A (not delete); refresh the retire view; re-upload via a fresh build (its `putBlob` observes A condemned → re-uploads a DISTINCT token B); reference+drop B; run to fixpoint; assert B is deleted.

```cpp
TEST(CasGcLeak, ResurrectReplacedIncarnationReclaimed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = makeStore(b);                 // same store construction the other CasGcLeak tests use
    const RootNamespace ns = testNamespace();
    Gc gc(s);
    const String P = "resurrect-payload";
    const BlobId pid = idOf(P);

    // 1. Publish ref R1 -> token A referenced; capture A.
    publishOneBlobPart(s, ns, "r1", P);
    const HeadResult hA = b->head(s->layout().blobKey(pid));
    ASSERT_TRUE(hA.exists);

    // 2. Drop R1 -> A dereferenced.
    s->dropRef(ns, "r1");

    // 3. ONE GC round: A transitions to in-degree 0 and is condemned (retired), NOT yet deleted.
    gc.runRegularRound();
    s->retireView().refresh();
    ASSERT_TRUE(s->retireView().isCondemnedToken(ObjectKind::Blob, u128Of(P), hA.token))
        << "precondition: token A must be condemned before the resurrect";
    ASSERT_TRUE(blobPresent(b, s->layout(), P)) << "A not yet deleted (still in the pipeline)";

    // 4. RESURRECT: a fresh build dedup-hits P; putBlob sees A condemned -> re-uploads a DISTINCT token B.
    publishOneBlobPart(s, ns, "r2", P);
    const HeadResult hB = b->head(s->layout().blobKey(pid));
    ASSERT_TRUE(hB.exists);
    ASSERT_NE(hB.token.value, hA.token.value) << "resurrect must mint a new incarnation token B";

    // 5. Drop R2 -> B dereferenced.
    s->dropRef(ns, "r2");

    // 6. Run GC to fixpoint. The replaced incarnation B MUST be reclaimed.
    runGcToFixpoint(s, gc);

    const FsckReport after = runFsck(*s, /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u);
    EXPECT_EQ(after.unreachable, 0u) << "the resurrect-replaced incarnation B must not orphan";
    EXPECT_FALSE(blobPresent(b, s->layout(), P)) << "B's object must be deleted";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of(P)), 0) << "no stranded positive in-degree";
}
```

Note: match `makeStore`/`testNamespace`/`ObjectKind`/`HeadResult`/`RoundReport` to how the other tests in this file / `cas_test_helpers.h` spell them (adjust includes/usings as those tests do). If `retireView().refresh()` is not the exact spelling, use the refresh call the writer path uses (`CasBuild.cpp:828` calls `store->retireView().refresh()`).

- [ ] **Step 2: Build the test binary.** From repo root, redirect to a log and summarize via a subagent:

```bash
ninja -C build unit_tests_dbms > build/build_orphan_test.log 2>&1
```
Expected: build succeeds (a subagent reads `build/build_orphan_test.log` and returns a one-line summary).

- [ ] **Step 3: Run the test to verify it FAILS (RED).**

```bash
build/src/unit_tests_dbms --gtest_filter='CasGcLeak.ResurrectReplacedIncarnationReclaimed' > build/test_orphan.log 2>&1; echo "exit=$?"
```
Expected: FAIL — `blobPresent` is TRUE and/or `unreachable != 0` (B orphaned). If the two `ASSERT_*` preconditions (A condemned, B distinct token) fail instead, the choreography is wrong — fix the setup until the test reaches the final `EXPECT_*` and fails THERE (the real leak), then proceed.

- [ ] **Step 4: Commit the RED test.**

```bash
git add src/Disks/tests/gtest_cas_gc_leak.cpp
git commit -m "test(cas): failing test for RESURRECT-REUPLOAD-ORPHAN (resurrect-replaced incarnation leaks)"
```

---

### Task 2: Add the `blob_retire_replaced` event + `CasGcRetireReplaced` counter + `RetiredMergeResult.replaced`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h` (enum), `.../Core/CasEvent.cpp` (name)
- Modify: `src/Common/ProfileEvents.cpp` (counter)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h` (`RetiredMergeResult`)

**Interfaces:**
- Produces: `CasEventType::BlobRetireReplaced` → `"blob_retire_replaced"`; `ProfileEvents::CasGcRetireReplaced`; `RetiredMergeResult.replaced` (a `std::vector<RetiredEntry>`).

- [ ] **Step 1: Add the event type.** In `CasEvent.h`, extend the enum line — add `BlobRetireReplaced` after `BlobRetire`:

```cpp
    BlobPut, BlobReuseAdopt, BlobReuseResurrect, BlobCopyForward, BlobRetire, BlobRetireReplaced, BlobDelete, BlobForget,
```

- [ ] **Step 2: Add the event name.** In `CasEvent.cpp`, after the `BlobRetire` case:

```cpp
        case CasEventType::BlobRetireReplaced:    return "blob_retire_replaced";
```

- [ ] **Step 3: Add the counter.** In `src/Common/ProfileEvents.cpp`, next to the other `CasGc*` entries (near `CasGcCas`):

```cpp
    M(CasGcRetireReplaced, "CA gc re-condemns of a resurrect-replaced incarnation (the current object token differed from a prior retired entry; the stale entry was superseded)", ValueType::Number) \
```

- [ ] **Step 4: Add the `replaced` result field.** In `CasBlobInDegree.h`, add to `struct RetiredMergeResult` after `redelete`:

```cpp
    std::vector<RetiredEntry> replaced;   /// re-condemned CURRENT tokens that superseded a stale entry (resurrect-replaced); caller emits blob_retire_replaced
```

- [ ] **Step 5: Build to confirm it compiles (no behavior change yet).**

```bash
ninja -C build unit_tests_dbms > build/build_orphan_scaffold.log 2>&1
```
Expected: compiles (subagent summarizes the log).

- [ ] **Step 6: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.cpp src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h
git commit -m "cas: add blob_retire_replaced event, CasGcRetireReplaced counter, RetiredMergeResult.replaced field"
```

---

### Task 3: Implement the `closeBlob` supersede rule + caller emit → GREEN

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp` (`closeBlob`, ~L232)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` (caller: emit event + counter for `merge.replaced`, ~L318 near the `graduated` loop)

**Interfaces:**
- Consumes: `head_blob`, `condemn_round` (already parameters of `foldDeltasIntoGeneration`); `RetiredMergeResult.replaced` (Task 2); `RetiredEntry{kind, hash, token, size, condemn_round}`.
- Produces: superseded re-condemn entries in `rmr.still_retired` (so they enter the pipeline) and `rmr.replaced` (so the caller logs them).

- [ ] **Step 1: Edit `closeBlob`'s prior-entry branch.** Replace the current `if (ri < prior_retired.size() && prior_retired[ri].hash == cur_blob) settleEntry(prior_retired[ri++], cur_edges);` with the supersede-aware version:

```cpp
        if (ri < prior_retired.size() && prior_retired[ri].hash == cur_blob)
        {
            /// RESURRECT-REUPLOAD-ORPHAN: on a re-reference cycle (touched this window, net in-degree 0),
            /// re-observe the CURRENT token. If it differs from the retired entry's token, a resurrect
            /// replaced the incarnation at this key — supersede the stale entry with a fresh condemn of the
            /// current token so the replacement enters the pipeline (the stale token's exact-token delete
            /// would only find the new token and no-op). Keyed on (hash, current token), matching GRetire.
            bool superseded = false;
            if (cur_edges == 0 && cur_touched && head_blob)
            {
                if (const auto hr = head_blob(cur_blob);
                    hr && hr->exists && hr->token != prior_retired[ri].token)
                {
                    RetiredEntry fresh;
                    fresh.kind = ObjectKind::Blob;
                    fresh.hash = cur_blob;
                    fresh.token = hr->token;
                    fresh.size = hr->size;
                    fresh.condemn_round = condemn_round;
                    rmr.replaced.push_back(fresh);              /// caller emits blob_retire_replaced
                    rmr.still_retired.push_back(std::move(fresh));
                    ++ri;                                       /// drop the stale entry (superseded)
                    superseded = true;
                }
            }
            if (!superseded)
                settleEntry(prior_retired[ri++], cur_edges);
        }
```

- [ ] **Step 2: Emit the event + counter in the caller.** In `CasGc.cpp`, right after the `for (const RetiredEntry & entry : merge.graduated)` loop (~L318-321), add a `merge.replaced` loop that mirrors how `BlobRetire`/`GcRetireObserve` events are built in this function (copy the field-setting shape used at ~L611/L627). Concretely:

```cpp
        for (const RetiredEntry & entry : merge.replaced)
        {
            ProfileEvents::increment(ProfileEvents::CasGcRetireReplaced);
            auto & e = <the CA-log event sink used by the graduated/retire loop in this scope>;
            e.type = CasEventType::BlobRetireReplaced;
            e.object_kind = ObjectKind::Blob;
            e.object_hash = entry.hash;
            e.token = entry.token.value;
            e.gen = generation;                 /// same generation var the retire loop uses
            e.round = new_round;                /// same round var the retire loop uses
            e.outcome = "replaced";
            e.reason = "current object token differs from the retired entry — resurrect replaced the "
                       "incarnation; superseded the stale entry and re-condemned the current token";
        }
```
Match the exact event-sink append idiom to the surrounding `graduated`/`BlobRetire` code (variable names `generation`, `new_round`, and the log-append helper are whatever that function already uses). If unclear, read `CasGc.cpp:596-640` and copy the pattern verbatim.

- [ ] **Step 3: Build.**

```bash
ninja -C build unit_tests_dbms > build/build_orphan_fix.log 2>&1
```
Expected: compiles (subagent summarizes).

- [ ] **Step 4: Run the RED test — now GREEN.**

```bash
build/src/unit_tests_dbms --gtest_filter='CasGcLeak.ResurrectReplacedIncarnationReclaimed' > build/test_orphan_green.log 2>&1; echo "exit=$?"
```
Expected: PASS (`blobPresent`==false, `unreachable`==0, `inDegree`==0).

- [ ] **Step 5: Run the whole CasGcLeak suite (no regressions).**

```bash
build/src/unit_tests_dbms --gtest_filter='CasGcLeak.*' > build/test_gcleak_all.log 2>&1; echo "exit=$?"
```
Expected: all PASS (subagent summarizes the log).

- [ ] **Step 6: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp
git commit -m "cas: re-condemn resurrect-replaced incarnation in the fold (fix RESURRECT-REUPLOAD-ORPHAN)"
```

---

### Task 4: Idempotency test — re-condemn does not churn or duplicate

**Files:**
- Modify: `src/Disks/tests/gtest_cas_gc_leak.cpp`

**Interfaces:**
- Consumes: same helpers as Task 1; `RoundReport` (has per-round counters, e.g. `graduated`/`redeleted`).

- [ ] **Step 1: Write the test.** After reclaim, extra rounds must be no-ops for this hash (no re-condemn, no duplicate entry):

```cpp
TEST(CasGcLeak, ResurrectReplacedReclaimIsIdempotent)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = makeStore(b);
    const RootNamespace ns = testNamespace();
    Gc gc(s);
    const String P = "resurrect-payload-idem";

    publishOneBlobPart(s, ns, "r1", P);
    s->dropRef(ns, "r1");
    gc.runRegularRound();
    s->retireView().refresh();
    publishOneBlobPart(s, ns, "r2", P);          // resurrect -> token B
    s->dropRef(ns, "r2");
    runGcToFixpoint(s, gc);
    ASSERT_FALSE(blobPresent(b, s->layout(), P)); // reclaimed

    // Extra rounds: nothing left to do for this hash.
    const RoundReport r = gc.runRegularRound();
    EXPECT_FALSE(blobPresent(b, s->layout(), P)) << "stays deleted";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of(P)), 0);
    // No re-condemn on an already-reclaimed hash (report has no new retire/graduate for it).
    const FsckReport after = runFsck(*s, /*detail=*/false);
    EXPECT_EQ(after.unreachable, 0u);
    EXPECT_EQ(after.dangling, 0u);
}
```
(Adjust `RoundReport` field checks to the report's actual fields; the load-bearing assertions are `blobPresent==false` staying stable and `unreachable==0` across extra rounds.)

- [ ] **Step 2: Build + run.**

```bash
ninja -C build unit_tests_dbms > build/build_orphan_idem.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasGcLeak.ResurrectReplacedReclaimIsIdempotent' > build/test_orphan_idem.log 2>&1; echo "exit=$?"
```
Expected: PASS.

- [ ] **Step 3: Commit.**

```bash
git add src/Disks/tests/gtest_cas_gc_leak.cpp
git commit -m "test(cas): resurrect-replaced reclaim is idempotent (no re-condemn churn)"
```

---

### Task 5: Writer-side test — the resurrect-replaced token is condemned in the retire view

**Files:**
- Modify: `src/Disks/tests/gtest_cas_gc_leak.cpp`

**Interfaces:**
- Consumes: `s->retireView().isCondemnedToken(ObjectKind::Blob, hash, token)`.

- [ ] **Step 1: Write the test.** After the fold re-condemns B, a dedup-hit on the hash must see B condemned (so writers resurrect, not adopt the being-reclaimed B):

```cpp
TEST(CasGcLeak, ResurrectReplacedTokenIsCondemnedInRetireView)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = makeStore(b);
    const RootNamespace ns = testNamespace();
    Gc gc(s);
    const String P = "resurrect-payload-view";
    const BlobId pid = idOf(P);

    publishOneBlobPart(s, ns, "r1", P);
    s->dropRef(ns, "r1");
    gc.runRegularRound();
    s->retireView().refresh();
    publishOneBlobPart(s, ns, "r2", P);                 // resurrect -> token B
    const HeadResult hB = b->head(s->layout().blobKey(pid));
    s->dropRef(ns, "r2");

    // The round that folds B's dereference re-condemns token B.
    gc.runRegularRound();
    s->retireView().refresh();
    EXPECT_TRUE(s->retireView().isCondemnedToken(ObjectKind::Blob, u128Of(P), hB.token))
        << "the replaced incarnation B must be visible as condemned so writers resurrect, not adopt";
}
```
(If B is already physically deleted by the time the view refreshes at these bounds, assert instead that `blobPresent==false` — either outcome proves B is not adoptable. Keep whichever the harness timing yields; the invariant is "B is never an adoptable live token".)

- [ ] **Step 2: Build + run.**

```bash
ninja -C build unit_tests_dbms > build/build_orphan_view.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasGcLeak.ResurrectReplacedTokenIsCondemnedInRetireView' > build/test_orphan_view.log 2>&1; echo "exit=$?"
```
Expected: PASS.

- [ ] **Step 3: Commit.**

```bash
git add src/Disks/tests/gtest_cas_gc_leak.cpp
git commit -m "test(cas): resurrect-replaced token is condemned in the retire view (writer resurrects, not adopts)"
```

---

### Task 6: Full-suite regression + scenario regression + docs

**Files:**
- Read: `build/test_*` logs
- Modify: `utils/ca-soak/scenarios/BACKLOG.md`, `docs/superpowers/cas/06-tla-models.md`

**Interfaces:** none (verification + docs).

- [ ] **Step 1: Run the broader CA GC unit suites (no regressions).**

```bash
build/src/unit_tests_dbms --gtest_filter='CasGc*:CasBlobIndegree*:CasFsck*:CasRetireView*' > build/test_ca_regression.log 2>&1; echo "exit=$?"
```
Expected: all PASS (subagent summarizes; investigate any failure before proceeding).

- [ ] **Step 2: Scenario regression (S30).** Rebuild the CA binary and rerun the deterministic S30 repro; it must lose its `unaccounted` residual.

```bash
ninja -C build clickhouse > build/build_clickhouse.log 2>&1
cd utils/ca-soak && python3 -m scenarios.run --scenario S30 --scale dev --seed 20260707 > logs/s30_postfix.log 2>&1; cd -
```
Expected: S30 `no unbounded leftovers` verdict PASS (residual drains to 0; a subagent checks the run's `report.json`). Clean rustfs after (`docker compose down -v --remove-orphans` + `docker volume prune -f`).

- [ ] **Step 3: Update docs (per spec §Docs to update).** Mark `06-tla-models.md` §Area 12 as "C++ fix landed — model is now a regression gate"; mark `utils/ca-soak/scenarios/BACKLOG.md` `RESURRECT-REUPLOAD-ORPHAN` resolved (S30 green).

- [ ] **Step 4: Commit.**

```bash
git add docs/superpowers/cas/06-tla-models.md utils/ca-soak/scenarios/BACKLOG.md
git commit -m "docs(cas): RESURRECT-REUPLOAD-ORPHAN fix landed — model now a regression gate; backlog resolved"
```
