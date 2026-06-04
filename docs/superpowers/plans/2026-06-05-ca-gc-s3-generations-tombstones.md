# CA GC Convergence — S3: Generations + Tombstones Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`). Build to a log (`ninja -C build … > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Tests FOREGROUND, bounded (`timeout` ≤ 590), non-empty `--test`, never `clickhouse local`, never background a build/test.

> **⚠️ RISK — DATA LOSS / ATTENDED REVIEW REQUIRED.** S3 **changes deletion semantics**: it replaces the bare-key direct `removeObjectsIfExist` with a generationed seal → grace → fresh-recheck → sweep protocol, and introduces the GC-owned `.tombstone` barrier. A bug here is **data loss** (a swept-but-still-referenced blob/manifest → an unreadable committed part). This stage **REQUIRES attended review of the race oracles** (the §11 S3 oracles: seal/resurrect/gravestone-lineage, active+sweep-reset+reader-fallback, tombstone-does-not-block-reads, mark/recover/**drain**/sweep) **before shipping**. The `gc_lock` is **still held** in S3 (it is the carrier that keeps the §7 handshake race closed); S3 only adds the **tomb barrier** that S4 will need once the lock is dropped. Do not conflate S3's barrier with S4's lock removal — ship them separately, S4 only after S3 + the oracles are proven.

**Goal:** Make a lockless, unconditional delete **ABA-safe** (spec §6) by adding **generations** on **both** `blobs/<H>/<g>` and `parts/<part_id>/<mg>` (manifests are handled symmetrically — §9), a **GC-owned `.tombstone`** (one object, three fates: seal → recover/un-seal, or → permanent gravestone), a best-effort `active` preferred-generation hint, **resurrection** to `g+1`/`mg+1` on a contended writer, and the **mark / recover / drain / sweep** GC state machine (§6) replacing the direct delete (G2). The manifest *body* still pins **bare `H`** (so `part_id` stays a pure content function — dedup/idempotency unchanged); generation lives only in the physical key and the delta log. Tombstone **gates attachment, not reads** (§6.1): a reader that `GET`s a sealed-but-present blob uses it regardless of the tombstone; only a `404` triggers `LIST` fallback. Safety in S3 rests on the **still-held `gc_lock` + the new tomb barrier**.

**Architecture:** Generation suffixes are added at the object-storage boundary (`PoolPaths`), so a `(H,g)` / `(part_id,mg)` is a typed key; the manifest serialization is untouched (it stores bare `H`). The conditional-create primitive (`PoolCoordination::condCreateIfAbsent`, S3 `If-None-Match` / local `O_EXCL`) is reused to create `blobs/<H>/<g>`, seal `<g>.tombstone`, and seal `parts/<part_id>/<mg>.tombstone`. The S2 compaction's count-0 candidate becomes the input to a GC state machine: **seal → grace → fresh authoritative re-check (§6.2) → {sweep | recover | drain}**. The writer's commit path (built in S2/S4) gains a **tomb re-check + resurrect** loop. Generations are symmetric for blobs and manifests — the same seal/grace/recheck/sweep machinery handles both, closing the relink-after-full-drop ABA hole (§9).

**Spec:** `docs/superpowers/specs/2026-06-04-ca-gc-convergence-design.md` (§6 generations/tombstones, §6.1 read-safety/`active`, §6.2 authoritative re-check, §8 I4/I7a–d, §9 manifest reclamation + failures, §11 S3 tests). **Branch:** `cas-mergetree-poc`. Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- `PoolPaths.{h,cpp}`: `blobKey(prefix, H)` (`:19`) → gains a generation: `blobGenKey(prefix, H, g)` + `blobTombstoneKey(prefix, H, g)` + `blobActiveKey(prefix, H)`; symmetric `partManifestGenKey(prefix, part_id, mg)` + `partTombstoneKey(...)` + `partActiveKey(...)`.
- `PoolCoordination::condCreateIfAbsent` (`PoolCoordination.cpp:234`) — the single-owner create used to make `blobs/<H>/<g>` (I5 uniqueness) and to **seal** `<g>.tombstone` / `<mg>.tombstone` (single-owner seal). The fence (`leadership_lost`/fence-still-mine) gates the GC-owned seal/delete.
- `ContentAddressedGC::runSweepOnce` (`ContentAddressedGC.cpp:301`): the candidate (from S2 compaction) → S3 state machine; the **direct delete** (~`:442` `removeObjectsIfExist`) is replaced by seal/grace/recheck/sweep. `grace`/`firstUnreachable` ageing (`:214`) stays liveness-only (never a safety fence — §6.3).
- The §6.2 authoritative re-check reads the **live session set** (`sessionPinnedBlobs`/`sessionPinnedPartKeys`, `ContentAddressedGC.cpp:~150-205`, `WriteSession.*`) + the current compaction's post-seal `LIST` — a single pass (ephemeral session `+`s suppress candidacy in the merge).
- Writer commit path: `ContentAddressedTransaction::commit`/`commitOnePart` gains the **tomb re-check + resurrect** loop; the relink path (`ContentAddressedMetadataStorage.cpp:101-259`) is already §7-shaped (pin-before-publish) and is the model.
- Read path: `ContentAddressedMetadataStorage::getStorageObjects`/`resolveBlobEntry`/`existsFile` resolve `g` via `blobActiveKey` (default 0) with the `404` → `LIST` fallback; reader caches the resolved `g` on the node (§6.1, §12.4).
- `GcDelta` (S2): the `+` records the resolved `(H,g)` and `(part_id, mg)` (the `event_id` generation discriminator reserved in S2 is now used).

---

## Phase 1 — generationed keys + read path (the common g=0 path unchanged)

### Task 1: generation suffixes on blob + manifest keys

**Files:** Modify `…/ContentAddressed/PoolPaths.{h,cpp}`, `…/Identifiers.h`

- [ ] **Step 1:** add `blobGenKey(prefix, H, g)` → `blobs/<H0>/<H1>/<H>/<g>`, `blobTombstoneKey(prefix, H, g)` → `…/<g>.tombstone`, `blobActiveKey(prefix, H)` → `…/active`. Symmetric: `partManifestGenKey(prefix, part_id, mg)` → `parts/<part_id>/<mg>`, `partTombstoneKey(prefix, part_id, mg)`, `partActiveKey(prefix, part_id)`. Keep `g=0`/`mg=0` the common path. Typed keys (B29).
- [ ] **Step 2:** a `parseGenFromKey` + `maxGenForHash(LIST blobs/<H>/)` helper so generation lineage (`max(gen)`) is reconstructable from surviving `<g>`/`.tombstone` objects even if `active` is lost (§6 last bullet, I7d).
- [ ] **Step 3:** bump `PoolMeta` version 2 → 3 (no back-compat — §1; reject an older-version pool, fail-closed). `PoolMeta.{h,cpp}`.
- [ ] **Step 4: build** `ninja -C build clickhouse > build/gcs3_t1_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/gcs3_t1_build.log` → 0.
- [ ] **Step 5: commit** `CA GC S3: generationed blob+manifest keys (symmetric) + PoolMeta v3`.

### Task 2: read path resolves generation via `active` with 404→LIST fallback

**Files:** Modify `…/ContentAddressedMetadataStorage.{h,cpp}`

- [ ] **Step 1:** `getStorageObjects`/`resolveBlobEntry` resolve a blob's `g`: read `blobActiveKey` (default 0, absent in the common case — readers assume `g=0`) → produce `blobGenKey(H, g)`. Symmetric for the manifest via `partActiveKey` (default 0). The steady `g=0` read is **one `GET`** — no `active` read, no `LIST` (§12.4).
- [ ] **Step 2:** **404 → fallback** (§6.1 reader rule): on a `GET` miss, `LIST blobs/<H>/`, pick any present generation (prefer highest/current — byte-identical, I7c), use its bytes, and opportunistically repair `active` (best-effort PUT, no CAS). Cache the resolved `g` on the node so a `g>0` (resurrected) content does not re-pay the round-trip per read.
- [ ] **Step 3:** **tombstone does NOT block reads** (§6.1, I7b/c): a successful `GET` of a sealed-but-present generation **uses the bytes regardless of any `.tombstone`** — only a `404` triggers fallback. Do not read the tombstone on the read path at all.
- [ ] **Step 4: build** → 0 errors.
- [ ] **Step 5: commit** `CA GC S3: read path resolves generation via active (404→LIST fallback, tombstone never blocks reads)`.

---

## Phase 2 — writer tomb re-check + resurrection

### Task 3: writer tomb re-check + resurrect to g+1 / mg+1

**Files:** Modify `…/ContentAddressedTransaction.cpp` (commit/`commitOnePart`)

- [ ] **Step 1:** resolve `H → g` per blob and `part_id → mg` for the manifest via the respective `active` (default 0). Upload missing `blobGenKey(H,g)` and the manifest `partManifestGenKey(part_id,mg)` via `condCreateIfAbsent` (I5).
- [ ] **Step 2:** **re-check** `blobTombstoneKey(H,g)` for every pinned `(H,g)` **and** `partTombstoneKey(part_id,mg)`. Note (§12.1): the tomb re-check is only needed for **reused** blobs — a freshly created `(H,g)` has no prior `+`, so the compaction can never have produced it as a candidate, so it cannot be sealed. So the re-check is per **reused** `(H,g)`, served by one prefix `LIST blobs/<H>/` (existence + tombstone + active in one op).
- [ ] **Step 3:** if any tomb is present → **resurrect** (§6, doc §8.2): `condCreateIfAbsent(blobGenKey(H, g+1))` → best-effort advance `active → g+1` → re-pin / re-log `(H, g+1)` → retry the re-check. Never wait, never rescue `g`, never delete the tombstone, never re-upload to the sealed key. Symmetric `mg+1` for the manifest.
- [ ] **Step 4:** the `+` delta (S2) now carries the **resolved** `(H,g)` and `(part_id,mg)` — logged **after** the tomb re-check (so an abandoned generation never leaves a stale `+`). NOTE: in S3 the session is the durable flag conceptually, but the lock is still held — the full session-is-the-flag ordering (re-check **before** the `+`) is S4; in S3 keep the ordering correct (re-check before `+`) but the `gc_lock` still serializes commit vs sweep.
- [ ] **Step 5: build** → 0 errors.
- [ ] **Step 6: commit** `CA GC S3: writer tomb re-check + resurrect to g+1/mg+1 (per reused (H,g), one prefix LIST)`.

---

## Phase 3 — GC mark / recover / drain / sweep state machine

### Task 4: seal → grace → fresh authoritative re-check (§6.2)

**Files:** Modify `…/ContentAddressedGC.{h,cpp}`

- [ ] **Step 1:** a candidate `(H,g)` (from the S2 compaction count-0 stream) → **seal**: `condCreateIfAbsent(blobTombstoneKey(H,g))` (single-owner, fence-gated). Once sealed, no new attachment may target `g` (I4) — the writer's re-check (Task 3) routes reuse to `g+1`.
- [ ] **Step 2:** **grace** (liveness only — never a safety fence, §6.3): keep the existing `firstUnreachable`/`grace` ageing as a delay knob.
- [ ] **Step 3:** **fresh authoritative re-check (§6.2)** performed **after** the seal: *no live session pins `(H,g)`* **and** *no `+` for `(H,g)` in the log tail since the last fold*. Implement as a **single pass**: live session pins are folded into the streaming merge as **ephemeral `+`s** (suppress candidacy, NOT written to the snapshot) — a `(H,g)` any live session pins simply never falls out as a candidate. Cost is O(merge), not O(sessions × candidates); the session set is read once per compaction.
- [ ] **Step 4: build** → 0 errors.
- [ ] **Step 5: commit** `CA GC S3: candidate → seal → grace → fresh authoritative re-check (single-pass, ephemeral session +s)`.

### Task 5: the recover / drain / sweep branches

**Files:** Modify `…/ContentAddressedGC.{h,cpp}`

- [ ] **Step 1:** after the re-check, branch (§6 / ChatGPT review):
  - **no ref/session for `g`** → **SWEEP**: delete `blobGenKey(H,g)`, reset `active` off `g` if it pointed there (§6.1 best-effort PUT), **keep the gravestone** `<g>.tombstone` (permanent). This is the only delete; it is unconditional + ABA-proof because `g` is sealed (I4).
  - **ref/session for `g`, and NO successor generation exists** → **RECOVER**: delete `<g>.tombstone` (un-seal), re-open `g` as the attachable generation.
  - **ref/session for `g`, but a successor `g+1…` already exists** → **DRAIN**: keep `<g>.tombstone` AND keep `blobGenKey(H,g)` (still referenced — do NOT delete), do NOT re-open `g`; new attaches go to the successor; `g` is swept later once its pins drain to zero. This is the multi-generation reality (I7a) — two byte-identical generations pinned at once, only one attachable.
- [ ] **Step 2:** **manifests are symmetric** (§9): a candidate `(part_id, mg)` whose count hit 0 runs the **identical** seal/grace/recheck/sweep + resurrection machinery (NOT a same-key delete — that has the relink-after-full-drop ABA hole). Re-creation after a sweep routes to `mg+1` (a different key), so the manifest delete is unconditional-safe by the same proof as I4. Factor the state machine so it operates on a generic `(id, gen)` key family and is instantiated for both blobs and manifests.
- [ ] **Step 3:** batch the seals/deletes per epoch (multi-object delete) so reclamation throughput is not a sequential-op problem (§12.2).
- [ ] **Step 4: build** → 0 errors.
- [ ] **Step 5: commit** `CA GC S3: recover/drain/sweep branches (symmetric for blobs+manifests, gravestone kept)`.

---

## Phase 4 — gtests (race oracles), regression, finalize

### Task 6: per-stage gtests + the §11 S3 race oracles (NO sleeps)

**Files:** Modify `src/Disks/tests/gtest_content_addressed*.cpp`

- [ ] **Step 1 — seal / resurrect / gravestone-lineage:** seal `(H,0)`; a writer reusing `H` resurrects to `(H,1)`; assert `active` advanced (best-effort), the gravestone `(H,0).tombstone` persists after sweep, and `maxGenForHash` reconstructs lineage from surviving objects even with `active` removed.
- [ ] **Step 2 — best-effort `active` + sweep-resets-`active` + reader fallback:** sweep `(H,0)` while `(H,1)` is live → `active` reset off 0 → a reader with a stale `active=0` `GET`s `404` → `LIST blobs/<H>/` → reads `(H,1)` → repairs `active`. Assert the read succeeds and `active` is repaired.
- [ ] **Step 3 — tombstone-does-not-block-reads:** seal `(H,0)` but do NOT sweep it; a reader `GET`s `(H,0)` successfully and **uses the bytes regardless of the tombstone** (assert no fallback fired, no exception).
- [ ] **Step 4 — mark/recover/drain/sweep transitions** (deterministic interleavings):
  - *recover:* candidate sealed, then a live session pins `g`, no successor → assert RECOVER (tombstone deleted, `g` re-attachable).
  - *drain (the load-bearing branch — "successor exists → drain, don't recover"):* W1 pins `g`, GC seals `g`, W2 resurrects `g+1`, GC re-check finds W1 still pins `g` AND `g+1` exists → assert **DRAIN** (keep `<g>.tombstone` + keep `blobGenKey(H,g)`, do NOT re-open `g`), then drop W1 → `g` count hits 0 → swept later, gravestone kept.
  - *sweep:* candidate sealed, no ref/session → SWEEP (blob deleted, gravestone kept).
- [ ] **Step 5 — seal→resurrect→GC-recovers-old-generation (I7a, two generations pinned):** exercise the §6.1 scenario where BOTH `g` and `g+1` are pinned at once; assert reads are byte-correct from either generation (NOT the false "single live generation" lemma).
- [ ] **Step 6 — `active`/default points at a tombstoned-but-present generation during grace:** assert a reader still succeeds (tombstone gates attachment, not reads).
- [ ] **Step 7 — manifest symmetry:** repeat the seal/resurrect/sweep oracle for `(part_id, mg)` — assert a relink-after-full-drop routes to `mg+1` and the old `mg` sweep cannot kill the re-created manifest (ABA hole closed).
- [ ] **Step 8: build + run** `ninja -C build unit_tests_dbms > build/gcs3_t6_build.log 2>&1; echo $?; build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcs3_t6_run.log 2>&1; echo $?; tail -20 build/gcs3_t6_run.log` → all pass.
- [ ] **Step 9: commit** `CA GC S3: gtests — seal/resurrect/gravestone, active+fallback, tombstone-doesn't-block-reads, mark/recover/drain/sweep, manifest symmetry`.

### Task 7: observability counters (§13)

**Files:** Modify the GC/transaction code

- [ ] **Step 1:** export `cas_generation_resurrections_total`, `cas_duplicate_generation_bytes`, `cas_tombstones_total`, `cas_generations_per_hash` (p99), `cas_writer_tombstone_s3_fallbacks` — guardrails for hot content cycling zero-refs→resurrection (gravestones are safe but not free).
- [ ] **Step 2: build** → 0 errors. **commit** `CA GC S3: generation/tombstone observability counters`.

### Task 8: CA regression + ATTENDED REVIEW + backlog + push

- [ ] **Step 1:** CA-default smoke (foreground, `timeout 590`, non-empty `--test`): `04278_content_addressed_disk 04279_content_addressed_gc 04280_content_addressed_clone_partition_works 04292_content_addressed_mutations 05003_content_addressed_freeze 05004_content_addressed_transactions` → all pass. `04279_content_addressed_gc` MUST be green (reclamation now via seal/sweep). Subagent summarizes.
- [ ] **Step 2:** all `ContentAddressed*` gtests green.
- [ ] **Step 3 — ATTENDED REVIEW GATE:** request human review of the §11 S3 race oracles (Task 6 Steps 4–7) **before** declaring S3 shippable. S3 changes deletion semantics — a bug is data loss. Do NOT proceed to S4 until this review passes.
- [ ] **Step 4: backlog** — append a "CA GC S3 DONE" note: generations on `blobs/<H>/<g>` AND `parts/<part_id>/<mg>` (symmetric), GC-owned `.tombstone` seal+gravestone, best-effort `active` + sweep-reset + tombstone-doesn't-block-reads, resurrection to g+1/mg+1, mark/recover/**drain**/sweep replacing bare-key direct delete (G2). Safety = still-held `gc_lock` + the new tomb barrier. Reference the spec + this plan. Note the attended-review gate.
- [ ] **Step 5: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- Blobs and manifests are generationed and symmetric: `blobs/<H>/<g>`, `parts/<part_id>/<mg>`, each with a GC-owned `<g>.tombstone` (seal → recover/un-seal or → permanent gravestone) and a best-effort `active` hint. `PoolMeta` bumped to v3 (no back-compat).
- The steady `g=0` read is one `GET`; `active`+404→`LIST` fallback handles resurrected content; a tombstone never blocks a read (only `404` does).
- Writers re-check the tombstone for reused `(H,g)`/`(part_id,mg)` and resurrect to `g+1`/`mg+1`; freshly-created generations skip the re-check (cannot be sealed).
- The GC replaces the direct delete (G2) with candidate → seal → grace → **fresh authoritative re-check (§6.2, single-pass, ephemeral session +s)** → {sweep | recover | **drain**}; the gravestone is kept on sweep; manifests run the identical machinery (relink ABA hole closed).
- gtests cover all §11 S3 oracles incl. successor-exists→drain, two-generations-pinned, and manifest symmetry; observability counters exported. The **attended-review gate** on the race oracles is satisfied.
- CA suite + all `ContentAddressed*` gtests green. `gc_lock` still held (no lock removal — that is S4). Backlog notes S3 done + the data-loss-risk / attended-review flag.
