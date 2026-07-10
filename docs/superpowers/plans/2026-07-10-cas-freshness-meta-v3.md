# CAS per-hash freshness meta (v3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Supersedes** `docs/superpowers/plans/2026-07-10-cas-meta-descriptor-raw-body.md` (the raw-body / terminal-tombstone plan — REJECTED, see spec §raw-body-refinement v3). This v3 plan keeps the settled one-key-per-hash + in-body `incarnation_tag` + exact-token BODY delete and adds ONLY a freshness meta.

**Goal:** Replace the writer-side `RetireView` retired-list download with a per-hash `.meta` freshness point-read (`{clean, condemned}`), while leaving the validated exact-token BODY delete core (`CaIncarnationCore`) and the read path unchanged.

**Architecture:** Every blob body keeps its in-body `incarnation_tag` (one key per hash; the tag varies the etag so a resurrect displaces the body and GC's exact-token `DELETE If-Match` misses — the unchanged safety core). A new tiny per-hash `blobs/xx/<hash>.meta` = `{version, state∈{clean,condemned}, condemn_round, size}` is a **freshness marker only**, not a linearizer: the writer point-reads it at dedup-adoption (condemned ⇒ resurrect, else adopt) instead of downloading the `O(condemned-set)` retired list. GC writes the meta `condemned` at condemn (before it can delete), clears it at spare, deletes it after the body delete. The writer-side `RetireView`/syncer/`observed_gc_round`/ack-floor are then deleted.

**Tech Stack:** ClickHouse C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), gtest (`unit_tests_dbms`), TLC/TLA+ (`docs/superpowers/models/`), CA-s3 stateless + `utils/ca-soak`.

## Global Constraints

- **Branch discipline:** work on `cas-gc-rebuild`; add new commits, never rebase/amend/force; never commit to `master`. (CLAUDE.md)
- **Design authority:** `docs/superpowers/specs/2026-07-09-cas-writer-gc-simplification-design.md` §raw-body-refinement (v3), §meta-layout (v3), §meta-protocols (v3). The v3 protocol table is normative.
- **KEEP the safety core:** one key per hash; in-body `incarnation_tag`; exact-token BODY delete (`DELETE If-Match <condemn-time token>`); resurrect overwrites the body with a fresh tag. This is `CaIncarnationCore` — do NOT change it. Never introduce per-incarnation body keys (the rejected generation-in-key design), a tombstone state, or a read-path offset change.
- **The meta is advisory freshness, NOT the delete linearizer.** Delete-safety stays on the body's exact-token delete. A momentarily-stale meta may only make a writer resurrect conservatively; it must never let a writer adopt a doomed body — so GC writes the meta `condemned` before any delete, and the writer point-reads it with a strongly-consistent GET (S3 RAW consistency — a pool requirement; note it in comments).
- **No compat scaffolding / no fallback paths:** CAS is pre-release; adding the meta changes the pool layout — dev pools are recreated, no migration, no dual read path, no config flag. (spec §non-goals; CLAUDE.md)
- **GC never throws/fail-closes on a 404 during fold** (record + continue). (`feedback_ca_gc_never_throw_on_404`)
- **Never GET a condemned body to revive it** — resurrect overwrites from the writer's own source; the ONE exception is the tokenless copy-forward of a committed-source dep (reads its still-present condemned body). (`feedback_ca_resurrect_invariant`)
- **Mass-DROP:** GC's per-hash meta condemn/clear/delete writes MUST run on a bounded parallel pool (1M condemns sequential ≈ hours). The existing exact-token body delete already paces per-object; the pool serves both.
- **C++ style:** Allman braces; "exception" not "crash"; function names as `f`; ASan (two words). (CLAUDE.md)
- **Build/test hygiene:** never pass `-j`/`nproc`; redirect ninja to `build/<name>_build.log` and each test to a unique `build/test_<name>.log`; summarize logs via a subagent. (CLAUDE.md)

---

## File Structure

**Landed (Task 1 family — to be REVISED to 2-state in Task 1):**
- `Core/CasBlobMeta.{h,cpp}` — `BlobMeta` + codec + meta-ops layer (already committed `ba883680114`/`c7481ab91db` as 3-state+nonce; Task 1 trims it to 2-state, no nonce).
- `Core/CasLayout.h` — `blobMetaKey` (landed).
- `src/Disks/tests/gtest_cas_blob_meta.cpp`, `cas_test_helpers.h` — meta tests + helpers (landed; adjusted in Task 1).

**Modified:**
- `Core/CasBuild.{h,cpp}` — replace `store->retireView().isCondemnedToken(...)` with a meta point-read; write the meta `{clean}` on fresh upload and on resurrect; K3 re-source (Tasks 3, 4). The envelope/`incarnation_tag`/`putOverwrite`-fresh-tag body path is UNCHANGED.
- `Core/CasGc.cpp` — condemn writes meta `condemned` (+ existing ledger retire); spare clears meta; delete deletes meta after the existing exact-token body delete; on a parallel pool; `graduationDue` re-keyed to `condemn_round < current_round` (Task 5). The exact-token body delete (`CasGc.cpp:274`) and `peek_head` supersede are UNCHANGED.
- `Core/CasStore.{h,cpp}`, `Core/CasRetireView.{h,cpp}`, `Core/CasServerRoot.{h,cpp}` — delete the writer-side `RetireView`, syncer, `observed_gc_round`, `min_ack` graduation gating (Task 6).
- `Core/CasInspect.cpp`, `Core/CasFsck.{h,cpp}` — `.meta` inspect dispatch + meta⟺body pairing (Task 2).

**Not touched (unlike the raw-body plan):** the read path / `locate` / `blob_header_len` (stays); the manifest format (stays pure content); the blob envelope body layout (stays — optional slimming is a separate follow-up, not this plan).

---

## Task 0: TLA — wedge gate (done) + the meta-freshness obligation

**Files:** `docs/superpowers/models/CaManifestSweepWindow.tla` (+ cfgs, DONE); a short written argument in the spec for the meta-freshness obligation, or a tiny model if preferred.

**Interfaces:** No C++. Establishes the formal footing: v3's delete-safety is the UNCHANGED `CaIncarnationCore` (no new model); the only new obligation is that the meta point-read is at least as fresh, for the K1 dedup-adoption gate, as the retire-view it replaces.

- [x] **Step 1: Wedge gate** — DONE (`8606ab382aa`): `CaManifestSweepWindow_reduced` GREEN, `sab_sweep_committed` RED (`INV_FOLD_PROGRESS`).

- [ ] **Step 2: Discharge the meta-freshness obligation (written argument)**

Add a short subsection to the spec (§meta-protocols) stating and arguing: *GC writes the meta `condemned` (durable) strictly before the two-phase delete can execute the exact-token body delete (condemn at round R, delete at ≥R+2); a writer's strongly-consistent GET of the meta after R observes `condemned`; therefore at the instant a writer adopts a `clean`/absent meta, the body's current token is not yet condemned, so the exact-token delete cannot target the adopted incarnation before the writer's precommit edge folds (EDGE-BEFORE-OBSERVE).* This is strictly weaker than the retire-view (which delivered the same fact via a list); no new delete-safety model is required — cite `CaIncarnationCore`'s `sab_unconddelete`/`sab_noretireview` controls. (If a model is preferred over prose, a 1-hash model with a `clean`-meta-point-read replacing the retire-view suffices; optional.)

- [ ] **Step 3: Commit** the spec argument.
```bash
git add docs/superpowers/specs/2026-07-09-cas-writer-gc-simplification-design.md
git commit -m "docs(cas): discharge the v3 meta-freshness obligation (>= retire-view for K1)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 1: Trim `BlobMeta` to the 2-state freshness marker

**Files:**
- Modify: `Core/CasBlobMeta.h`, `Core/CasBlobMeta.cpp`
- Modify: `src/Disks/tests/gtest_cas_blob_meta.cpp`, `cas_test_helpers.h`

**Interfaces:**
- Consumes: the landed `CasBlobMeta` (3-state + `incarnation` nonce) and its ops layer (`loadMeta`/`putMetaIfAbsent`/`casMeta`/`deleteMetaExact`, `LoadedMeta`), `Layout::blobMetaKey`.
- Produces (final v3 shape):
  - `enum class MetaState : uint8_t { Clean = 0, Condemned = 1 };` (drop `Tombstone`).
  - `struct BlobMeta { uint8_t version = 1; MetaState state = MetaState::Clean; uint64_t condemn_round = 0; uint64_t size = 0; };` (drop the `incarnation` nonce — the meta is not the linearizer, so its etag need not be globally unique; `casMeta`/`putMetaIfAbsent`/`deleteMetaExact` still use the backend etag for their conditional ops).
  - Codec `encodeBlobMeta`/`decodeBlobMeta` over the trimmed struct (magic `"CAMT"`, decode fails closed on bad magic/version/length).
  - The ops-layer signatures are unchanged.

**Why:** v3's meta is a freshness marker, not a linearization point, so the `Tombstone` state (v2 terminal tombstone — rejected) and the `incarnation` nonce (v2 finding #8 — only needed when the meta etag was the linearizer) are both unnecessary. Removing them keeps the codec minimal and the semantics honest.

- [ ] **Step 1: Update the round-trip test to the 2-state struct**

In `gtest_cas_blob_meta.cpp`, change the codec round-trip test to iterate `{MetaState::Clean, MetaState::Condemned}` only, and remove the `incarnation`/`FreshIncarnationMakesEachWriteUnique` test:
```cpp
TEST(CasBlobMeta, CodecRoundTripsBothStates)
{
    for (MetaState s : {MetaState::Clean, MetaState::Condemned})
    {
        BlobMeta m{.version = 1, .state = s, .condemn_round = 42, .size = 1 << 20};
        const BlobMeta back = decodeBlobMeta(encodeBlobMeta(m));
        EXPECT_EQ(static_cast<uint8_t>(back.state), static_cast<uint8_t>(s));
        EXPECT_EQ(back.condemn_round, 42u);
        EXPECT_EQ(back.size, 1u << 20);
    }
}
```

- [ ] **Step 2: Run to verify it fails (Tombstone / incarnation still referenced)**
```bash
ninja -C build unit_tests_dbms > build/cas_meta_v3_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobMeta.*' > build/test_cas_meta_v3.log 2>&1
```
Expected: FAIL/compile error (the old tests reference `MetaState::Tombstone` / `incarnation`).

- [ ] **Step 3: Trim `CasBlobMeta.h`** — remove `Tombstone` from `MetaState`; remove `UInt128 incarnation` from `BlobMeta`; update the doc comment (2-state freshness marker; not the linearizer).

- [ ] **Step 4: Trim `CasBlobMeta.cpp`** — remove the `incarnation` (16 bytes) from `BODY_LEN` and the encode/decode; keep the `Clean`/`Condemned` mapping; keep decode fail-closed (`state > Condemned` ⇒ `CORRUPTED_DATA`).

- [ ] **Step 5: Update helpers** — in `cas_test_helpers.h`, drop the test-only `mintU128()` and the `.incarnation = mintU128()` in `writeMetaClean`/`condemnMeta` (no longer a field). `condemnMeta` sets `state = Condemned`, `condemn_round`.

- [ ] **Step 6: Build + run**
```bash
ninja -C build unit_tests_dbms > build/cas_meta_v3_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobMeta.*' > build/test_cas_meta_v3.log 2>&1
```
Expected: all `CasBlobMeta.*` pass.

- [ ] **Step 7: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.cpp \
        src/Disks/tests/gtest_cas_blob_meta.cpp src/Disks/tests/cas_test_helpers.h
git commit -m "refactor(cas): trim BlobMeta to the 2-state freshness marker (v3)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 2: `ca-inspect` `.meta` dispatch + `ca-fsck` meta⟺body pairing

**Files:** `Core/CasInspect.cpp`, `Core/CasFsck.{h,cpp}`, `gtest_cas_blob_meta.cpp`, `gtest_cas_fsck.cpp`.

**Interfaces:**
- Consumes: `decodeBlobMeta`, `Layout::blobMetaKey`, `caInspectToJson` (`CasInspect.h:25`), `FsckReport` (`CasFsck.h:44`), the `blobs/` LIST at `CasFsck.cpp:188`.
- Produces: `ca-inspect` renders a `.meta` key as JSON (state/condemn_round/size). `FsckReport` gains `uint64_t meta_without_body` (INV violation — a condemned/clean meta with no body) and `uint64_t body_without_meta` (benign — a not-yet-condemned or crashed-birth body; NOT a dangle). `clean()` includes `meta_without_body == 0`.

- [ ] **Step 1: Failing inspect test** — in `gtest_cas_blob_meta.cpp`, `caInspectToJson(layout, blobMetaKey, encodeBlobMeta({condemned,...}))` must return JSON containing `"condemned"` (currently throws — the `.meta` key matches the `blobs/` envelope branch at `CasInspect.cpp:422`).

- [ ] **Step 2: Run to verify fail** (`ninja … > build/cas_inspect_v3_build.log`; run `--gtest_filter='CasBlobMeta.Inspect*'`). Expected: throws `CORRUPTED_DATA`/`BAD_ARGUMENTS`.

- [ ] **Step 3: Add the `.meta` branch BEFORE the `blobs/` branch** in `caInspectToJson` (`CasInspect.cpp:422`):
```cpp
    if (key.starts_with(layout.blobsPrefix()) && key.ends_with(".meta"))
        return renderBlobMeta(decodeBlobMeta(bytes));
```
`renderBlobMeta` mirrors the file's existing `render*` JSON idiom: `{object:"blob_meta", version, state:"clean"|"condemned", condemn_round, size}`. Add `#include ".../CasBlobMeta.h"`. The body branch (non-`.meta`) is UNCHANGED — bodies keep their envelope, so `decodeEnvelopeHeader` still applies (no raw-body change in v3).

- [ ] **Step 4: Run inspect test** → PASS.

- [ ] **Step 5: Failing fsck pairing tests** — in `gtest_cas_fsck.cpp`: a `.meta` with no body ⇒ `rep.meta_without_body >= 1`; a body with no `.meta` ⇒ `rep.body_without_meta >= 1` and `rep.dangling == 0`.

- [ ] **Step 6: Run to verify fail** (fields don't exist).

- [ ] **Step 7: Implement** — add the two counters to `FsckReport` (and `clean()` includes `meta_without_body == 0`); in `CasFsck.cpp`, partition the `blobs/` LIST (`:188`) into bodies vs `.meta` keys (they share the prefix), and after the existing passes add the pairing loop (reuse the hash-parse idiom from `CasGc.cpp:1838-1849`). Ensure `.meta` keys are excluded from the reachable-body classification so they aren't themselves flagged dangling.

- [ ] **Step 8: Build + run** `--gtest_filter='CasFsck.*'` → new pass; existing fsck tests still green.

- [ ] **Step 9: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp \
        src/Disks/tests/gtest_cas_blob_meta.cpp src/Disks/tests/gtest_cas_fsck.cpp
git commit -m "feat(cas): ca-inspect .meta dispatch + ca-fsck meta<->body pairing (v3)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 3: Writer — point-read the meta instead of the retire-view; write the meta

**Files:** `Core/CasBuild.cpp` (`putBlob:130`, `uploadFromSource:311`, `observeAndAdmit:227/250`), `Core/CasBuild.h`; tests in `gtest_cas_build.cpp`.

**Interfaces:**
- Consumes: `loadMeta`/`putMetaIfAbsent`/`MetaState` (Task 1); the EXISTING body path (`uploadFromSource` with `putOverwrite` fresh `incarnation_tag`, `observeAndAdmit` HEAD, `store->retireView().isCondemnedToken(...)`).
- Produces (spec §meta-protocols v3):
  - The condemned check `store->retireView().isCondemnedToken(kind, hash, token)` (`CasBuild.cpp:270/449/528`) is replaced by a meta point-read: `loadMeta(hash)` → `state == Condemned`.
  - Fresh upload writes the meta `{clean, size}` (`putMetaIfAbsent`) after the body PUT succeeds.
  - Resurrect (the existing `putOverwrite` fresh-tag path in `uploadFromSource`) additionally writes the meta `{clean}` after displacing the body.
  - `absent` meta + present body ⇒ adopt (best-effort create meta `{clean}`) — an absent meta means "not condemned" (GC writes `condemned` before deleting).
- The `incarnation_tag` / envelope / `putOverwrite` body mechanics are UNCHANGED.

- [ ] **Step 1: Failing tests** — in `gtest_cas_build.cpp`:
  - `PutBlobFreshUploadWritesCleanMeta`: after `putBlob` of new content, `loadMeta(hash).state == Clean`.
  - `PutBlobAdoptsWhenMetaCleanNoRetireView`: with a pre-existing body + `writeMetaClean`, a second `putBlob` adopts (no `putOverwrite`), meta stays clean.
  - `PutBlobResurrectsWhenMetaCondemned`: with body present + `condemnMeta`, `putBlob` displaces the body (new token) AND sets the meta clean. (Assert via `CountingBackend` that a `putOverwrite` happened and the meta is clean.)

- [ ] **Step 2: Run to verify fail** (`ninja … > build/cas_build_v3_build.log`; `--gtest_filter='CasBuild.PutBlob*'`). Expected: no meta written; condemned decided via retire-view.

- [ ] **Step 3: Replace the condemned check with a meta point-read.** At each `store->retireView().isCondemnedToken(kind, hash, hr.token)` site (`CasBuild.cpp:270`, `:449`; and the copy-forward gate `:528` is Task 4), replace with:
```cpp
    const auto lm = loadMeta(store->backend(), store->layout(), hash);
    const bool condemned = lm && lm->meta.state == MetaState::Condemned;
```
Keep the surrounding logic (condemned ⇒ the existing resurrect/`putOverwrite` fresh-tag path; else adopt). The `e.round = store->retireView().round()` event fields become a fixed 0 or are dropped (the round is a GC concept now sourced from the meta's `condemn_round` when relevant).

- [ ] **Step 4: Write the meta on fresh upload + resurrect.** In `uploadFromSource` (`CasBuild.cpp:311`): after the fresh body PUT succeeds, `putMetaIfAbsent(hash, {clean, size})` (a 412 is fine — a racing writer created it). After the condemned-displacement `putOverwrite` (the resurrect path, `:475`), `putMetaIfAbsent`-or-`casMeta` the meta to `{clean}` (overwrite the condemned meta: `loadMeta` then `casMeta(lm->etag, {clean})`; on conflict re-load and follow). In `observeAndAdmit` (the adopt path), if `loadMeta` returned absent, best-effort `putMetaIfAbsent(hash, {clean, size})` so future readers point-read it.

- [ ] **Step 5: Run** `--gtest_filter='CasBuild.PutBlob*'` → PASS. (Cross-suite retire-view tests stay red until Task 6.)

- [ ] **Step 6: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/tests/gtest_cas_build.cpp
git commit -m "feat(cas): writer point-reads the freshness meta (replaces retire-view) + writes it (v3)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 4: Promote K3 — meta point-read; copy-forward writes the meta

**Files:** `Core/CasBuild.cpp` (`promote` K3 loop `:878-909`, `copyForwardFromCondemned:506`), tests in `gtest_cas_build.cpp`/`gtest_cas_protocol_scenarios.cpp`.

**Interfaces:**
- Consumes: `loadMeta`/`MetaState`; the existing `copyForwardFromCondemned` (GET condemned body + re-wrap fresh tag + `putOverwrite`) and `isCopyForwardableTokenless`/`depIsTokened`.
- Produces: the K3 non-tokened-leaf revalidation replaces `store->retireView().isCondemnedToken(...)` (`CasBuild.cpp:894`) with a meta point-read; `copyForwardFromCondemned` additionally writes the meta `{clean}` after it displaces the body. Tokened leaves stay skipped (EDGE-BEFORE-OBSERVE). The body-side copy-forward mechanics are UNCHANGED.

- [ ] **Step 1: Failing tests** — `PromoteSkipsTokenedLeaves` (tokened leaf ⇒ no meta GET); `PromoteCopyForwardsCondemnedTokenless` (tokenless leaf + `condemnMeta` ⇒ promote succeeds, body displaced, meta clean); `PromoteAbortsOnAbsentBodyNonTokenedLeaf` (no body ⇒ `ABORTED`).

- [ ] **Step 2: Run to verify fail** (`--gtest_filter='CasBuild.Promote*:CasProtocol.*'`).

- [ ] **Step 3: Re-source the K3 check.** In the K3 loop (`CasBuild.cpp:894`), replace `store->retireView().isCondemnedToken(ObjectKind::Blob, e.blob_hash, hr.token)` with `loadMeta(...).state == Condemned`. Keep: `!hr.exists ⇒ ABORTED`; condemned + `isCopyForwardableTokenless ⇒ copyForwardFromCondemned`; condemned + no-dep ⇒ `ABORTED`.

- [ ] **Step 4: `copyForwardFromCondemned` writes the meta.** After the `putOverwrite(key, bytes_out, hr.token)` displacement (`CasBuild.cpp:576`), set the meta `{clean}` (`casMeta`/`putMetaIfAbsent`). The GET+verify+re-wrap+putOverwrite is UNCHANGED (INV-1 committed-source exception).

- [ ] **Step 5: Run** `--gtest_filter='CasBuild.Promote*:CasProtocol.*'` → PASS.

- [ ] **Step 6: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_build.cpp src/Disks/tests/gtest_cas_protocol_scenarios.cpp
git commit -m "feat(cas): promote K3 + copy-forward use the freshness meta (v3)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 5: GC — write the meta at condemn/spare/delete (on a parallel pool); re-key graduation

**Files:** `Core/CasGc.cpp` (condemn `head_blob:629`, delete site `:269-303`, `graduationDue:1523`), `Core/CasBlobInDegree.cpp` (`settleEntry:196`), tests in `gtest_cas_gc_ack_floor.cpp`/`gtest_cas_gc_leak.cpp`.

**Interfaces:**
- Consumes: `putMetaIfAbsent`/`casMeta`/`deleteMetaExact`/`MetaState`; the EXISTING ledger retire (`RetiredEntry{hash, token, condemn_round, ...}`), the EXISTING exact-token body delete (`CasGc.cpp:274`), `peek_head` supersede, `graduationDue`; a bounded `ThreadPool`.
- Produces:
  - **Condemn:** alongside the existing `head_blob` ledger capture (body token), write the meta `condemned` (`putMetaIfAbsent`-or-`casMeta` to `{condemned, condemn_round}`). This is the writer's freshness signal and MUST be durable before the delete pass.
  - **Spare:** where an entry is spared (`d>0`), clear the meta (`casMeta condemned→{clean}`).
  - **Delete:** after the existing `deleteExact(body, entry.token)` (`CasGc.cpp:274`) succeeds/returns, `deleteMetaExact`/delete the meta. NO tombstone. Idempotent (an already-absent meta is fine).
  - **Parallel pool:** the per-hash meta writes (condemn/spare/delete) run on a bounded `ThreadPool` (mass-DROP). The body-token exact delete already loops per-object; co-locate the meta op with each.
  - **Graduation:** re-key `settleEntry` (`CasBlobInDegree.cpp:208`) from `condemn_round < min_ack` to `condemn_round < current_round` where `current_round = new_round = state.round + 1` (finding #13 basis); remove `min_ack` from `foldDeltasIntoGeneration`. The `peek_head` body-token supersede is UNCHANGED (it operates on the body token, which v3 keeps).

- [ ] **Step 1: Failing tests** — `CondemnWritesMetaCondemned` (run a round on a d=0 blob ⇒ `loadMeta.state == Condemned`, body still present); `DeleteRemovesBodyAndMeta` (graduate + delete ⇒ body gone via exact-token, meta gone); `SpareClearsMeta` (recovered d>0 ⇒ meta clean).

- [ ] **Step 2: Run to verify fail** (`ninja … > build/cas_gc_v3_build.log`; `--gtest_filter='CasGcRetire.*:CasGcAckFloor.*'`).

- [ ] **Step 3: Condemn writes the meta.** In the condemn path (`head_blob` / `closeBlob` `CasBlobInDegree.cpp:271` where the `RetiredEntry` is built), after capturing the body token, write the meta `{condemned, condemn_round}` (via the pool in `runRegularRound`, or inline if that's where the round's writes live). Keep the existing ledger entry (body token) UNCHANGED.

- [ ] **Step 4: Delete writes the meta.** In the R3 redelete loop (`CasGc.cpp:269-303`), after the existing `deleteExact(blobKeyOf(layout, entry.hash), entry.token)`, delete the meta (`deleteMetaExact` or unconditional meta delete — the meta is advisory; deleting it after the body is safe). Submit both to the bounded pool. Never throw out of the pool (log + record anomaly).

- [ ] **Step 5: Spare clears the meta.** Where `settleEntry` spares (`rmr.spared`), schedule a `casMeta condemned→{clean}` (best-effort; a lost CAS = a writer already resurrected, fine).

- [ ] **Step 6: Introduce the bounded pool** — a `ThreadPool` member on `Gc` (size `gc_meta_pool_size`, default 16), used for the condemn/spare/delete meta ops (and co-located body delete). Collect futures before the round's `gc/state` CAS.

- [ ] **Step 7: Re-key graduation** — `condemn_round < current_round` (`current_round = new_round = state.round + 1`); drop `min_ack` from `foldDeltasIntoGeneration` + call sites; update `graduationDue`.

- [ ] **Step 8: Migrate GC tests** — `gtest_cas_gc_ack_floor.cpp` (drop `min_ack` assertions; assert meta transitions), `gtest_cas_gc_leak.cpp` (RESURRECT-REUPLOAD-ORPHAN — `peek_head` supersede still body-token-based, add a meta-clean assertion).

- [ ] **Step 9: Build + run** `--gtest_filter='CasGc*:CasThreeCursorMerge.*:CasBlobInDegree.*:CasReuseGcRace.*'` → PASS.

- [ ] **Step 10: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp \
        src/Disks/tests/gtest_cas_gc_ack_floor.cpp src/Disks/tests/gtest_cas_gc_leak.cpp
git commit -m "feat(cas): GC writes the freshness meta at condemn/spare/delete on a pool; graduate on rounds (v3)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 6: Delete the writer-side `RetireView` + syncer + `observed_gc_round` + ack-floor

**Files:** delete `Core/CasRetireView.{h,cpp}`, `gtest_cas_retire_view.cpp`; modify `Core/CasStore.{h,cpp}`, `Core/CasServerRoot.{h,cpp}`, `Core/CasGc.cpp` (round recovery), `Core/CasInspect.cpp` (mount-lease render).

**Interfaces:** Pure removal of the last writer-side view consumers (spec §phase-b-deletions). Precondition: Tasks 3-4 removed every `retireView()` read — verify by grep first.

- [ ] **Step 1: Grep gate** — `grep -rn "retireView\|retire_view\|observedGcRound\|observed_gc_round\|syncRetiredView\|RetireView\|min_ack" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ | grep -v "CasRetireView\.\|gtest_cas_retire_view"` returns only comments. Any live call = a prior task missed a re-source; fix there.

- [ ] **Step 2: Delete** `git rm Core/CasRetireView.{h,cpp} src/Disks/tests/gtest_cas_retire_view.cpp`.

- [ ] **Step 3: Remove from `CasStore`** — `observedGcRound` (`:290`/`:459`), `retireView()` (`:380`), `syncRetiredView`/`start|stopRetiredViewSync`/`retiredViewSyncLoop` (`:410/419/605-714`), `RetireView retire_view` (`:622`) + syncer thread state (`:657-661`), the `#include`, the prime (`:144`), syncer start (`:326`), dtor stop (`:348`), the `tryRemountOnce`/`renewWatermarkOnce` sync calls.

- [ ] **Step 4: Drop `observed_gc_round` from the beat + `MountLease`** — `CasServerRoot.{h,cpp}`: `MountLease.observed_gc_round`, `MountLeaseKeeper::observed_round_fn`, `prepareRenew` `value3`, `encodeBody`/codec; `CasStore.cpp` `observed_round_fn` wiring.

- [ ] **Step 5: Remove `min_ack`/`max_ack` + the 3 residual sites** — `HeartbeatFloor.min_ack/max_ack` (`CasServerRoot.cpp:479-562`); GC round recovery `std::max({max_fence_round, state.round, max_gen}) + 1` (`CasGc.cpp:1867-1872`); `RoundReport::min_ack` (`CasGc.h:80`/`.cpp:143/191`); `graduationDueForTest(min_ack)` (`CasGc.h:389`); `CasInspect.cpp:224` mount-lease `observed_gc_round` render.

- [ ] **Step 6: Migrate tests** — `gtest_cas_heartbeat.cpp`, `gtest_cas_store.cpp::CasStoreBeat`/`CasLeaseViewDecouple`, `gtest_cas_mount.cpp::CasHeartbeatFloor`: drop `observed_gc_round`/`min_ack`; keep `min_active`/expiry/fence.

- [ ] **Step 7: Build + run** `--gtest_filter='CasStore*:CasHeartbeat.*:CasMount*:CasHeartbeatFloor.*'` → PASS.

- [ ] **Step 8: Commit**
```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ src/Disks/tests/
git commit -m "refactor(cas): delete writer-side RetireView + syncer + observed_gc_round + ack-floor (v3)

The per-hash freshness meta point-read replaced the retired-list download; graduation paces on
GC rounds. Beat keeps lease + min_active.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 7: Integration validation — gtests, CA-s3 lane, soak

**Files:** test-only; `utils/ca-soak`.

- [ ] **Step 1: Full CAS gtest suite** — `ninja -C build unit_tests_dbms > build/cas_full_v3_build.log 2>&1`; `--gtest_filter='*Cas*:*Ca*'` → all pass (`CaWiring*` flaky set not grown). Investigate any failure with systematic-debugging.

- [ ] **Step 2: Deterministic freshness-race test** — a hand-driven `InMemoryBackend` test: GC condemns (meta condemned + ledger), a writer point-reads the meta → sees condemned → resurrects (body displaced, new token, meta clean), GC's exact-token delete of the OLD token → TokenMismatch (survives), no dangle. This is the C++ analog of `CaIncarnationCore`'s resurrect-vs-delete + the meta freshness.

- [ ] **Step 3: ASan** — `ninja -C build_asan …`; `--gtest_filter='*Cas*:*Ca*'` → no ASan reports (known CA-ASAN-SUITE LOGICAL_ERROR-abort debt aside).

- [ ] **Step 4: CA-s3 stateless lane** (per `reference_praktika_local_runs`) → 0 promote aborts, `ca-fsck` dangling=0, `meta_without_body=0`.

- [ ] **Step 5: Soak** (per `reference_ca_soak_fresh_restart`): `python3 -m soak.run --seed <N> --phase 3 --duration 1200` + a mass-DROP scenario sizing `gc_meta_pool_size`. Gate: fsck dangling=0, `meta_without_body=0`. Archive logs before teardown.

- [ ] **Step 6: Commit** the mass-DROP scenario + a worklog note.

---

## Self-Review

**Spec coverage (v3):** §meta-layout (Task 1 codec + `blobMetaKey` landed); §meta-protocols fresh-upload/adopt/resurrect/condemn/spare/delete (Tasks 3, 5); ca-inspect/ca-fsck (Task 2); §phase-b-deletions RetireView/syncer/observed_gc_round/ack-floor (Task 6); the meta-freshness obligation (Task 0); mass-DROP pool (Task 5); wedge gate (Task 0, done). The read path, manifest, envelope body, and exact-token delete are deliberately UNCHANGED (v3 keeps the validated core).

**What v3 drops vs the rejected raw-body plan:** the tombstone handshake, the terminal-wait, the read-path offset change, the manifest incarnation, per-incarnation keys, and the meta incarnation nonce — all gone. Lower risk, closer to `CaIncarnationCore`.

**Placeholder scan:** the `/* build+precommit */` markers in test steps point implementers to copy existing same-file boilerplate (the pattern exists in `gtest_cas_build.cpp`). No TBD/implement-later.

**Type consistency:** `MetaState{Clean,Condemned}`, `BlobMeta{version,state,condemn_round,size}`, `loadMeta`/`putMetaIfAbsent`/`casMeta`/`deleteMetaExact` are used identically across Tasks 1-5.
