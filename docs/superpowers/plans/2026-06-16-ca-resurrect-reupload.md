# CA B167 — resurrect re-stamps build_id + incremental GC honors the build heartbeat

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans or superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Close the bodyless publish-gate `resurrect`-vs-GC livelock (B167) so a merge/mutation that dedup-hits a condemned blob always converges — **without** retaining bodies in RAM (rejected: column blobs can be gigabytes) and **without** a GC→build back-link ("C", rejected).

**Architecture:** Two surgical parts, matching the revised spec and the rewritten TLA+ (`CaResurrectLiveness`, heartbeat-guard).
- **Part A (writer-side, 1 line):** `Build::resurrect` re-stamps the re-uploaded incarnation with the CURRENT build's `build_id` (it already mints a fresh `incarnation_tag` but preserves the decoded *old* `build_id`). `putBlob`/`putTree`/`recreateTree` already stamp `build_id`; only `resurrect` was missing it. This makes the resurrected blob *owned by the live build*.
- **Part B (incremental GC):** the condemn guard gains `∧ ¬liveBuild(build_id)`. At the R2 observe step, for a **Blob** candidate, GC reads the envelope `build_id` (a ranged GET of the fixed-length blob header) and **skips condemning** it while `builds/<build_id>` exists (the build is in-flight). This extends to incremental GC the in-flight protection the code's own comments already assume (`CasBuild.cpp:203-206`) and that full GC already gives debris.

Trees/packs are unmodified by Part B (tree headers are natural-length, not a fixed prefix; trees already converge via `recreateTree`'s retained payload). The huge-blob GET-buffer in `resurrect` is a *pre-existing*, separately-tracked `FOLLOW-UP(M-F)` (`CasBuild.cpp:292`) — out of scope here.

**Spec:** `docs/superpowers/specs/2026-06-16-ca-resurrect-reupload-design.md`. **Model:** `CaResurrectLiveness` (TLC: guard ON → `<>published` holds; guard OFF → violated).

**Build/test:**
- `cd build && ninja unit_tests_dbms > build_b167.log 2>&1` (analyze the log via subagent).
- `build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*'` (only the pre-existing B140 leak is expected red).

---

### Task 1: Part A — `resurrect` re-stamps the current `build_id`

**Files:** Modify `Core/CasBuild.cpp` (`Build::resurrect`, ~line 290-298).

- [ ] **Step 1: Write the failing test.** In `Disks/tests/gtest_cas_build.cpp` (grep for an existing `resurrect`/`Resurrect`/condemned-dep test and mirror its fixture; if none, use the `CasInMemoryBackend` + `Store` + `Build` setup the other `gtest_cas_build` tests use):

```cpp
TEST(CasBuild, ResurrectStampsCurrentBuildId)
{
    /// B167 Part A: resurrecting a condemned blob created by a PRIOR (dead) build must re-stamp the
    /// re-uploaded incarnation with THIS build's build_id, so the incremental-GC heartbeat guard (Part B)
    /// protects it. Before the fix, resurrect preserved the decoded old build_id.
    auto backend = std::make_shared<Cas::CasInMemoryBackend>();
    auto store = /* open a Store on `backend` exactly as the sibling tests do */;

    const Cas::UInt128 old_build = Cas::mintU128();
    const Cas::UInt128 new_build = Cas::mintU128();

    // 1) A prior build writes the blob (envelope build_id = old_build) and we drive it to CONDEMNED in
    //    the store's retire view (use the same retire-view stub the existing condemned-dep tests use).
    //    Produce a real object at the blob key with build_id = old_build.
    // 2) Open a Build with build_id = new_build.
    // 3) Call the resurrect path for that condemned (kind=Blob, hash) — via observeAndAdmit on a
    //    dedup-hit, or putBlob with a matching BlobSource (whichever the sibling tests exercise).
    // 4) GET the blob key and decode the envelope header.
    Cas::EnvelopeHeader h = Cas::decodeEnvelopeHeader(got.bytes, got.bytes.size(), Cas::ObjectKind::Blob);
    EXPECT_EQ(h.build_id, new_build);     // re-stamped to the live build
    EXPECT_NE(h.build_id, old_build);
}
```

- [ ] **Step 2: Run it, verify it FAILS** (`build/src/unit_tests_dbms --gtest_filter='*ResurrectStampsCurrentBuildId*'`) — expected: `build_id` still equals `old_build`.

- [ ] **Step 3: Implement.** In `Build::resurrect`, right after the existing `header.incarnation_tag = mintU128();` / `header.pad_to_header_len = header.header_len;` lines (CasBuild.cpp ~296-297), add:

```cpp
    /// B167 Part A: re-stamp the CURRENT build's build_id (resurrect decoded the OLD owner's header).
    /// The re-uploaded incarnation must be OWNED by this live build so the incremental-GC heartbeat
    /// guard (Part B) refuses to re-condemn it in the upload->publish span. Without this the incarnation
    /// would carry a prior (likely dead) build_id and get no protection — the B167 livelock.
    header.build_id = build_id;
```

- [ ] **Step 4: Build + run the test, verify it PASSES.** `ninja dbms > build_b167.log 2>&1` then the gtest filter above. Expected: PASS.

- [ ] **Step 5: Commit** `CA B167 (Part A): resurrect re-stamps the current build_id`.

---

### Task 2: Part B — incremental GC honors the build heartbeat for blob candidates

**Files:** Modify `Core/CasGc.cpp` (`Gc::retire` observe loop, ~626-644) and `Core/CasGc.h` (declare the helper).

- [ ] **Step 1: Write the failing tests** in `Disks/tests/gtest_cas_gc_round.cpp` (mirror the existing condemn/retire round tests' fixture — `CasInMemoryBackend`, a `Store`, a `Gc`, and a snap with a zero-in-degree blob candidate):

```cpp
TEST(CasGcRound, SkipsCondemnOfBlobOwnedByLiveBuild)
{
    /// B167 Part B: a zero-in-degree blob whose envelope build_id has a LIVE builds/<id> heartbeat must
    /// NOT be condemned (it is in-flight). Condemn guard: present /\ everEdged /\ InDeg=0 /\ ~liveBuild.
    // setup: write a blob with build_id = B; create builds/<B> (HeartbeatKeeper::start or a raw putIfAbsent
    //        of an encodeHeartbeat body at layout.buildHeartbeatKey(hex(B))); build a snap whose
    //        zeroInDegreeKnown() yields that (Blob, hash).
    auto retired = gc.retire(state, state_token, snap);
    // assert: NO retired entry for that (kind=Blob, hash) — it was skipped.
    EXPECT_TRUE(/* retired sets contain no entry for hash */);
}

TEST(CasGcRound, CondemnsBlobWhenHeartbeatGone)
{
    /// Same blob, but NO builds/<B> object (the owning build is gone). It MUST be condemned as today.
    auto retired = gc.retire(state, state_token, snap);
    EXPECT_TRUE(/* retired sets contain exactly one entry for hash */);
}
```

- [ ] **Step 2: Run them, verify the FIRST FAILS** (today GC condemns regardless of heartbeat) and the second passes.

- [ ] **Step 3: Declare the helper** in `Core/CasGc.h` (private section of `Gc`):

```cpp
    /// B167 Part B: is this present BLOB owned by an in-flight build? Reads the envelope build_id (a
    /// ranged GET of the fixed-length blob header) and HEADs builds/<build_id>. A live owner ⇒ the blob
    /// is in-flight ⇒ incremental GC must NOT condemn it this round (deferral is non-destructive; full
    /// GC's heartbeat-staleness still governs eventual debris reclaim). Blobs only — tree headers are
    /// natural-length. Returns false on build_id == 0 / decode failure (fail-open to condemn is wrong;
    /// fail-CLOSED here means "treat as not-live" so a corrupt header cannot pin an object forever —
    /// the recheck re-validates in-degree before any delete, so a spurious condemn is still safe).
    bool blobOwnedByLiveBuild(const Layout & layout, const Backend & backend,
                              const String & key, uint64_t object_size) const;
```

- [ ] **Step 4: Implement the helper** in `Core/CasGc.cpp` (near the other `Gc::` round helpers). Use the confirmed APIs: `store->poolMeta().blob_header_len`, `backend.get(key, Range{0, blob_header_len})`, `decodeEnvelopeHeader(bytes, object_size, ObjectKind::Blob)`, `layout.buildHeartbeatKey(u128ToHex(build_id))`, `backend.head(...)`:

```cpp
bool Gc::blobOwnedByLiveBuild(const Layout & layout, const Backend & backend,
                              const String & key, uint64_t object_size) const
{
    const uint64_t header_len = store->poolMeta().blob_header_len;
    if (object_size < header_len)
        return false;   /// corrupt/truncated — let the normal (recheck-guarded) condemn path handle it
    const std::optional<GetResult> head_bytes = backend.get(key, Range{0, header_len});
    if (!head_bytes)
        return false;   /// vanished between the observe HEAD and this read — nothing to protect
    EnvelopeHeader header;
    try
    {
        header = decodeEnvelopeHeader(head_bytes->bytes, object_size, ObjectKind::Blob);
    }
    catch (const Exception &)
    {
        return false;   /// undecodable header — do not let it pin the object; recheck still guards delete
    }
    if (header.build_id == UInt128{})
        return false;   /// no owner recorded (legacy/zeroed) — not protected
    return backend.head(layout.buildHeartbeatKey(u128ToHex(header.build_id))).exists;
}
```
(Verify `Range{0, header_len}` matches the `struct Range` fields in `CasBackend.h:12` — adjust the brace-init to its actual members, e.g. `{.offset=0, .length=header_len}`.)

- [ ] **Step 5: Wire the guard** into the observe loop in `Gc::retire`. After the `if (!observed.exists) continue;` block (CasGc.cpp ~631-635) and before building the `RetiredEntry`, add:

```cpp
            /// B167 Part B: do not condemn a blob that is still in-flight (its build_id heartbeat is
            /// live). This adds the 4th condemn guard ~liveBuild(build_id) to present /\ everEdged /\
            /// InDeg=0. Non-destructive: a still-live owner just defers this candidate to a later round
            /// (or to full GC's debris path once the heartbeat lapses). Blobs only.
            if (candidate.kind == ObjectKind::Blob
                && blobOwnedByLiveBuild(layout, backend, objectKey(layout, candidate.kind, candidate.hash), observed.size))
                continue;
```

- [ ] **Step 6: Build + run, verify BOTH tests PASS.** `ninja dbms > build_b167.log 2>&1`; `--gtest_filter='*CasGcRound*Condemn*:*CasGcRound*LiveBuild*'`.

- [ ] **Step 7: Commit** `CA B167 (Part B): incremental GC skips condemning a blob owned by a live build`.

---

### Task 3: convergence + dedup-preserved integration test

**Files:** `Disks/tests/gtest_cas_protocol_scenarios.cpp` (or wherever the multi-step protocol/gate scenarios live — grep for `gateCheckDeps`/`revalidateDeps`/publish scenarios).

- [ ] **Step 1: Write the test.** Drive the B167 scenario end-to-end on `CasInMemoryBackend` with a real `Gc`:
  1. Build1 writes a blob + publishes a tree referencing it; drop the ref so the blob goes zero-in-degree.
  2. Build2 (live heartbeat) `putBlob`s the SAME content (dedup-hit). Run a `Gc::retire` round concurrently/interleaved.
  3. Assert Build2 **publishes successfully** (no `ABORTED` livelock) and the published tree's blob is present.
  4. **Dedup-preserved contrast:** a dedup-hit on a still-LIVE (referenced) blob adopts free — assert no extra incarnation/overwrite occurred (token unchanged).

- [ ] **Step 2: Run, verify PASS.** `--gtest_filter='*B167*:*Resurrect*Converg*'`.

- [ ] **Step 3: Full CA suite.** `build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*'` → only the pre-existing B140 leak red. Use a subagent to analyze the log.

- [ ] **Step 4: Commit** `CA B167: convergence + dedup-preserved scenario test`.

---

### Task 4: docs — update the heartbeat-safety framing + backlog

**Files:** `Core/CasHeartbeat.h`, `docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md`, `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`.

- [ ] **Step 1: `CasHeartbeat.h:19-20`** — replace the "Heartbeats gate only DEBRIS reclamation by full GC" sentence with: heartbeats gate (a) full-GC debris reclamation, and (b) **incremental-GC condemnation of in-flight blobs** (B167) — incremental GC skips condemning a blob whose `build_id` heartbeat is live; the publish gate remains the commit-time safety mechanism.

- [ ] **Step 2: protocol spec** (`2026-06-10-ca-incarnation-store-design.md`, the "the gate, not the heartbeat, is the safety" lines ~384/565) — add a clause: in-flight blobs are *also* protected from incremental-GC condemnation by their live `build_id` heartbeat (B167); the gate remains the commit-time safety, and this protection is *deferral* (non-destructive), not timing-only protection — full GC's heartbeat-staleness still governs debris deletion.

- [ ] **Step 3: backlog** `cas-mergetree-integration.md` B167 row → mark implemented (Part A `resurrect` build_id stamp + Part B incremental-GC heartbeat guard; TLA+ `CaResurrectLiveness` guard ON holds / OFF violated; ships with B160). Note the op-count cost (ranged header GET + heartbeat HEAD per blob condemnation candidate) under B157.

- [ ] **Step 4: Commit** `CA B167: docs — heartbeat now co-gates incremental-GC condemnation`.

---

### Task 5 (after merge): soak validation (B160 + B167)
Rebuild `clickhouse`; run the two-replica soak with chaos off; confirm **0 broken detached parts** and a clean `fsck` (`dangling=0`) under productive GC — B160 (contention=0) + B167 (no broken parts) together.

## Notes
- No body retention anywhere; no GC→build back-link; no new GC state. Part B reads the blob's own header `build_id` + an existing heartbeat key.
- Op-count (B157): Part B adds, per **blob condemnation candidate** (rare — zero-in-degree blobs), one ranged header GET + one heartbeat HEAD. Bounded; far cheaper than streaming gigabytes twice.
- Fail-closed posture in `blobOwnedByLiveBuild`: any read/decode failure ⇒ "not live" ⇒ the normal condemn path runs, which is still recheck-guarded before any delete. A corrupt header can never pin an object forever.
