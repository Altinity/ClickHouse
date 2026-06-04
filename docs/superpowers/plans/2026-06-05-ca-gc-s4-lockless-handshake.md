# CA GC Convergence — S4: Lockless Handshake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`). Build to a log (`ninja -C build … > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Tests FOREGROUND, bounded (`timeout` ≤ 590), non-empty `--test`, never `clickhouse local`, never background a build/test.

> **⚠️ RISK — DATA LOSS / ATTENDED REVIEW REQUIRED / DO-NOT-MERGE-UNTIL-PROVEN.** S4 **drops the in-process `gc_lock`** between `commit` and `runSweepOnce` (G1). This is the single act that lands non-blocking writers — and it is the first time **both** the §7 handshake race **and** the §5.1 concurrent-append race are exercised in production. A bug here is **data loss** (a swept-but-referenced blob → an unreadable committed part). **S4 MUST NOT be merged until §5.1 (log completeness under concurrent appends) + §6 (the tomb barrier) + the §7/§5.1 race oracles are proven by attended review.** **PREREQUISITE (hard):** the §5.1 machinery must already be in place and exercised — **epoch-close (fenced PUT) before fold**, **writer re-append on advance**, and **sessions-held-until-folded**. These are built in S2 (epoch-close + re-append, under the lock) and completed here (sessions-until-folded). Do not remove the lock until all three hold and the oracles are green.

**Goal:** Replace the shared in-process `gc_lock` with the doc's **two-flag lockless handshake** (spec §7). The **writer-side flag is the session pin** (not the `+`, not the visible live ref): writer order is **session → upload → recheck-tomb → `+` → live-ref** (§7.1), keeping the session **until the `+` is durably written AND folded** (§5.1 rule 3). The GC order is **seal → fresh authoritative ref-check (§6.2) → delete**. The chain `end(publish) ≤ start(recheck) < end(seal) ≤ start(refcheck) < end(publish)` (publish = the session pin) is unsatisfiable, so "writer commits to `g`" and "GC deletes `g`" cannot both hold — with **no lock and no transaction**, only read/list-after-write. Then **drop `gc_lock`** between `commit` and `runSweepOnce` (G1). Safety rests on **the §7 proof + sessions-until-folded + the fence lease** (the lock is gone). A later **optional** sub-phase adds the §12 Keeper acceleration (a pure bounded cache — tests pass WITH and WITHOUT Keeper).

**Architecture:** The session (`WriteSession` / `sessions/<id>`) becomes the synchronous durable handshake flag, written first; the `+` delta becomes **batched/async** (S2's `GcLogWriter`), the session covering the reference in the gap until the `+` is durable AND folded. The writer commit path (S2/S3) is reordered to §7.1's exact sequence and the tomb re-check moves **before** the `+`. The GC drops the `gc_lock` acquisition in `runSweepOnce`; the §6.2 fresh authoritative re-check (built in S3, reading the live session set + the current compaction) becomes the *sole* gate. The relink path (`ContentAddressedMetadataStorage.cpp:101-259`) already IS a §7-shaped handshake; S4 makes the normal commit path match it instead of leaning on the mutex. Session reaping is timer-safe because the gravestone is permanent (a reaped-then-resumed writer re-checks the tomb and resurrects — §7.3). The optional Keeper accelerator (§12.2) mirrors the hot derived set (ephemeral sessions, per-shard epoch, valid-negative tombstone cache, log-tail) with the **seal-to-Keeper-before-recheck** coherence rule; losing it degrades to the §12.1 S3-only floor, never to incorrectness.

**Spec:** `docs/superpowers/specs/2026-06-04-ca-gc-convergence-design.md` (§5.1 epoch protocol/log-completeness, §6.2 authoritative re-check, §7 handshake, §7.1 writer order, §7.3 sessions-until-folded + reaping, §8 I1–I8 + theorem, §9 failures, §11 S4 oracles, §12 cost model + Keeper). **Branch:** `cas-mergetree-poc`. Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- `ContentAddressedGC::runSweepOnce` (`ContentAddressedGC.cpp:301`): the `std::lock_guard<std::mutex> gc_guard(*gc_lock)` at `:310` is **dropped** between commit and sweep (the seal/recheck/sweep state machine from S3 + the §6.2 fresh re-check become the sole gate; the fence lease still gates GC-vs-GC).
- Writer commit: `ContentAddressedTransaction::commit`/`commitOnePart` — reordered to §7.1 (session first, recheck-before-`+`, ref last, session-until-folded). The `gc_lock` acquisition on the commit side is dropped too.
- `WriteSession` (`WriteSession.{h,cpp}`) / `sessions/<id>` + `sessionPinnedBlobs`/`sessionPinnedPartKeys` (`ContentAddressedGC.cpp:~150-205`): the session is the handshake flag; its lifetime extends to **folded**, not commit. A reaper enforces the §7.3 mechanical rule.
- S2 `GcLogWriter` + epoch-close + re-append (§5.1 rules 1–2) — the PREREQUISITE; S4 completes rule 3 (sessions-until-folded) and the folded-watermark.
- S3 §6.2 re-check (live session set + current compaction, single pass) — the sole post-lock gate.
- `PoolCoordination` fence lease — kept; deletes gated on "fence still mine" (the existing `leadership_lost` check generalizes, §7.2).
- Relink model: `ContentAddressedMetadataStorage.cpp:101-259` (pin-before-publish) — the existing §7-shaped path the normal path is made to match. The S4 interleaving oracle is **modelled on the relink-race gtest `c28411372fa`** (CAS replication Phase 2a relink primitive).

---

## Phase 0 — PREREQUISITE verification (do NOT skip)

### Task 0: prove §5.1 machinery is in place before touching the lock

**Files:** read-only audit + a gtest

- [ ] **Step 1:** confirm S2's **epoch-close (fenced PUT) before fold** and **writer re-append on advance** are wired and exercised, and S3's **tomb barrier** + §6.2 fresh re-check are in place. If any is missing, STOP — S4 cannot proceed.
- [ ] **Step 2:** add a gtest asserting the §5.1 **completeness invariant** under a forced epoch-close mid-append (still with the lock, as a control): every reference whose `+` targeted the closed epoch is folded, re-logged, or session-covered. This is the baseline the lock-removal must preserve.
- [ ] **Step 3: build + run** the gtest → green. **commit** `CA GC S4 prereq: assert §5.1 log-completeness invariant (control, lock still held)`.

---

## Phase 1 — sessions-until-folded + the §7.1 writer order

### Task 1: session lifetime = until folded (§5.1 rule 3, §7.3)

**Files:** Modify `…/ContentAddressed/WriteSession.{h,cpp}`, `…/ContentAddressedTransaction.cpp`, `…/ContentAddressedMetadataStorage.{h,cpp}`

- [ ] **Step 1:** define the **folded watermark**: a session's `+` deltas (by `event_id`) are "folded" once they appear in a durable snapshot (`gcSnapKey`). Add `isEventFolded(event_id, shard)` (check the latest snapshot / a folded-watermark cache) so the writer can tell when it is safe to delete its session.
- [ ] **Step 2:** retain `sessions/<id>` until **every** delta `event_id` is folded (not merely until the live ref is committed), so *sessions ∪ folded-snapshot* always covers every live reference (the §6.2 gate completeness premise). Abort-before-commit stays O(1) (drop the session, nothing was referenced).
- [ ] **Step 3:** **timer-safe reaper** (§7.3 mechanical rule): a background reaper may delete a session only if (a) its ref was never committed, **or** (b) every delta `event_id` is in a folded snapshot, **or** (c) a root-marker reconciliation reconstructed equivalent reachability. NEVER on a bare timer for a committed-but-unfolded session. (No `sleep` in C++ — the reaper is event/poll-driven on the folded watermark.)
- [ ] **Step 4: build** → 0 errors. **commit** `CA GC S4: session lifetime extends to folded (folded watermark + timer-safe reaper rule)`.

### Task 2: reorder the writer to the §7.1 sequence (session is the flag)

**Files:** Modify `…/ContentAddressedTransaction.cpp` (commit/`commitOnePart`)

- [ ] **Step 1:** the exact order (§7.1): **(1)** resolve `H→g`, `part_id→mg` via `active`; **(2)** create/extend `sessions/<id>` with the resolved `[(H,g)…]` and `(part_id,mg)` — **this is the handshake flag** (durable, GC-visible), written FIRST; **(3)** upload missing blobs + manifest via `condCreateIfAbsent`; **(4)** **re-check** `.tombstone` for every pinned `(H,g)` and `(part_id,mg)`; **(5)** if any tomb present → resurrect to `g+1`/`mg+1`, retry from (2); **(6)** settle the final pinset and enqueue the `gc/log +` (with its `event_id`) — **batched/async** (S2 `GcLogWriter`), NOT a synchronous pre-ref PUT; **(7)** write the **live ref** (the commit point); **(8)** keep the session until the `+` is durable AND folded (Task 1), then delete it.
- [ ] **Step 2:** the key change vs S3: the tomb re-check (step 4) is now **before** the `+` (step 6), and the **session** (step 2), not the `+`, is the synchronous durable reference flag. This makes the §7 proof's "publish" flag `A` the session pin, which precedes both the re-check and GC's §6.2 check.
- [ ] **Step 3:** **unlink/drop ordering (§7.1, bias to over-count):** remove/tombstone the live ref **before** appending the `-`. A crash between leaves a not-live part whose blob is briefly over-counted (safe, reconciled), never an under-count.
- [ ] **Step 4: build** → 0 errors. **commit** `CA GC S4: writer order session→upload→recheck→+→live-ref (session is the flag, + is async)`.

---

## Phase 2 — DROP the lock (G1)

### Task 3: remove `gc_lock` between commit and sweep

**Files:** Modify `…/ContentAddressedGC.cpp`, `…/ContentAddressedTransaction.cpp`, `…/ContentAddressedMetadataStorage.{h,cpp}`

- [ ] **Step 1:** drop the `std::lock_guard<std::mutex> gc_guard(*gc_lock)` in `runSweepOnce` (`:310`) and the corresponding commit-side lock. The **sole gate** for a delete is now: seal (S3) → grace → **§6.2 fresh authoritative re-check** (live session set + current compaction, single pass) → delete iff none AND tombstone intact AND **fence still mine** (`leadership_lost` generalizes — §7.2). Keep the fence lease for GC-vs-GC.
- [ ] **Step 2:** confirm the GC order is exactly **seal → §6.2 re-check → delete** (the seal precedes the re-check; the re-check reads the live session set which the writer raised before its own re-check). This is the §7 store-then-load on the GC side.
- [ ] **Step 3:** remove the now-dead `gc_lock` member if nothing else uses it (or leave it as a no-op with a comment that G1 retired it). Update any callers/threads (`ContentAddressedGCThread`).
- [ ] **Step 4: build** → 0 errors. **commit** `CA GC S4: drop the in-process gc_lock between commit and sweep (G1) — handshake is the sole gate`.

---

## Phase 3 — the race oracles (the load-bearing proof; NO sleeps)

### Task 4: the §7 interleaving oracle + the §5.1 append-as-epoch-folds oracle

**Files:** Modify `src/Disks/tests/gtest_content_addressed*.cpp` (modelled on the relink-race gtest `c28411372fa`)

- [ ] **Step 1 — §7 interleaving oracle:** drive every interleaving of writer (publish-session → recheck-tomb → `+` → live-ref) vs GC (seal → §6.2-refcheck → delete) at the seams; assert that **no committed ref to content `H` ever becomes unreadable** — i.e. the unsatisfiable chain holds: there is no interleaving where the writer commits to `g` AND the GC deletes `g`. Use a deterministic step-injection harness (the relink-race gtest pattern), NOT sleeps.
- [ ] **Step 2 — append-as-epoch-folds (§5.1, the model-C load-bearing oracle):** a writer's `+` lands as its epoch is closed/folded; assert the writer **re-appends** into the open epoch (rule 2) and/or its **session covers** the reference until folded (rule 3), so the blob is **never swept while the part is live**. This is the oracle the whole lock-removal rests on — it must be exhaustive over the close/append interleaving.
- [ ] **Step 3 — seal→resurrect→GC-recovers-old-generation (I7a):** GC recovers `g` while `g+1` exists → reads stay correct with **two** generations pinned.
- [ ] **Step 4 — active-points-at-tombstoned-but-present generation:** during grace, a reader whose `active` points at a sealed-but-present generation still `GET`s it successfully (tombstone gates attachment, not reads).
- [ ] **Step 5 — duplicate `+` across an epoch close:** a re-append lands the same `event_id` in two epochs; assert dedup-on-fold collapses it to one count (no over-count), or that reconciliation bounds the over-count.
- [ ] **Step 6 — drop-vs-reuse / reuse-vs-delete / concurrent-resurrection / rebuild-after-state-loss** (C4/C1/C3/C7, §11): deterministic interleavings, no sleeps; assert no dangling ref under every interleaving.
- [ ] **Step 7: build + run** `ninja -C build unit_tests_dbms > build/gcs4_t4_build.log 2>&1; echo $?; build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcs4_t4_run.log 2>&1; echo $?; tail -20 build/gcs4_t4_run.log` → all pass.
- [ ] **Step 8: commit** `CA GC S4: race oracles — §7 interleaving, append-as-epoch-folds, two-gen recover, tombstoned-but-present read, dup-+-across-close, C1/C3/C4/C7`.

### Task 5: op-count budgets (§12.3) — the S3-only floor

**Files:** Modify the GC/transaction code; modify the gtest

- [ ] **Step 1:** assert the §12.3 **S3-only degraded-mode floor**: a small-part commit issues **session PUT + live-ref PUT** as the synchronous control writes (the `+` is async), parallel data uploads, **1 prefix-`LIST` per distinct reused `H`** (existence + tombstone + active in one op), and **no per-blob `HEAD`/`GET active`** on the success path. The key counter `cas_s3_sequential_control_depth_per_commit` target is **≤2** in S3-only degraded mode.
- [ ] **Step 2:** assert the MUST-NOT list (§12.3): no per-blob S3 `HEAD` tomb checks, no per-blob `GET active`, no per-commit `gc/current_epoch` read storm, no un-batched per-commit `gc/log` object. Counters: `cas_writer_tombstone_s3_fallbacks`, `cas_log_batch_size`, `cas_s3_session_fallbacks`.
- [ ] **Step 3: build + run** → asserts green. **commit** `CA GC S4: op-count budget asserts (S3-only floor: ≤2 sequential control writes, 1 LIST/reused H)`.

---

## Phase 4 — OPTIONAL Keeper acceleration (§12.2) — pure cache, tests pass WITH and WITHOUT

> This is an **optional accelerator** (§12). Keeper holds **no durable state** — its loss degrades to the §12.1/§12.3 S3-only floor, never to incorrectness. Every test in Phases 0–3 MUST pass both **with Keeper enabled** and **with Keeper disabled** (the S3-only path). Build this only after the S3-only handshake is proven.

### Task 6: ephemeral session pins + per-shard epoch mirror

**Files:** New `…/ContentAddressed/GcKeeperCache.{h,cpp}` (optional, behind a config flag); modify the writer + GC

- [ ] **Step 1:** ephemeral Keeper session pins (pinset inline for small parts; descriptor + an S3 session object for large parts), removing the S3 session PUT/DELETE in normal mode. **Safety rule:** if the Keeper session is lost before the live ref is committed → **abort** (uncommitted ≠ durable truth); if Keeper drops mid-flight → re-establish a durable **S3** session before committing, or abort. GC reads the Keeper session set in normal mode; when Keeper is down GC pauses (it is background) and writers use S3 sessions — the two never disagree.
- [ ] **Step 2:** per-shard epoch mirror: `gcCurrentEpochKey(shard)` (S3 truth) mirrored to Keeper with a watch; writers read/watch Keeper instead of `GET`-ing S3 per commit. GC order: PUT S3 epoch → publish Keeper epoch → **then** fold. Writer falls back to the S3 epoch read on a Keeper miss.
- [ ] **Step 3: build** → 0 errors. **commit** `CA GC S4 (opt): Keeper ephemeral session pins + per-shard epoch mirror (pure cache, S3 fallback)`.

### Task 7: valid-negative tombstone cache (the safety-critical one) + log-tail mirror

**Files:** Modify `…/ContentAddressed/GcKeeperCache.{h,cpp}`, the writer, the GC compaction

- [ ] **Step 1:** valid-negative tombstone/generation cache: a *trusted negative* ("no seal for `(H,g)` up to fence F") lets the writer skip the per-blob tomb `LIST`. **Coherence rule (safety-critical):** GC publishes each seal to Keeper **before** its S3 §6.2 re-check, so the §7 proof holds with the Keeper seal as flag `M`. A stale-**positive** → a needless resurrection (wasteful, never unsafe); a stale-**negative** is prevented by a freshness fence — if the cache cannot **prove** the negative at a fresh-enough fence (or Keeper is unavailable), the writer **falls back to the S3 prefix `LIST`** (§12.1), counted by `cas_keeper_tombstone_negative_invalid` (measured, never hidden).
- [ ] **Step 2:** log-tail mirror: Keeper holds small per-delta metadata (`{s3_log_key, event_id, hash range}`), not full pinsets, so the compaction skips the S3 `LIST gc/log/...` in the common case; on Keeper loss it lists S3 and rebuilds. GC batches seals/deletes (multi-object delete) per epoch.
- [ ] **Step 3:** assert the §12.3 **1-PUT normal-mode floor** (`cas_s3_sequential_control_depth_per_commit` target **1** with Keeper); counters `cas_keeper_generation_cache_miss`, `cas_keeper_tombstone_negative_invalid`.
- [ ] **Step 4:** **run the full Phase 0–3 oracle + budget suite BOTH with Keeper enabled and disabled** — assert identical correctness; assert the budget shifts to 1 PUT with Keeper and ≤2 without. Keeper loss mid-test degrades to the S3-only floor, never to a dangling ref.
- [ ] **Step 5: build + run** → all pass with and without Keeper. **commit** `CA GC S4 (opt): valid-negative tombstone cache (seal-to-Keeper-before-recheck) + log-tail mirror — 1-PUT floor, tests pass with+without Keeper`.

---

## Phase 5 — regression, ATTENDED REVIEW, finalize

### Task 8: CA regression + the DO-NOT-MERGE-UNTIL-PROVEN gate + backlog + push

- [ ] **Step 1:** CA-default smoke (foreground, `timeout 590`, non-empty `--test`): `04278_content_addressed_disk 04279_content_addressed_gc 04280_content_addressed_clone_partition_works 04292_content_addressed_mutations 05003_content_addressed_freeze 05004_content_addressed_transactions` → all pass, WITH and WITHOUT Keeper. `04279_content_addressed_gc` MUST be green. Subagent summarizes.
- [ ] **Step 2:** all `ContentAddressed*` gtests green (incl. all Phase 3 race oracles).
- [ ] **Step 3 — DO-NOT-MERGE-UNTIL-PROVEN GATE (hard):** S4 drops the lock — a bug is data loss. **S4 must not be merged until §5.1 (log completeness under concurrent appends) + §6 (tomb barrier) + the §7/§5.1 race oracles are proven by attended human review.** Request that review explicitly; capture sign-off before declaring S4 shippable.
- [ ] **Step 4: backlog** — append a "CA GC S4 DONE" note: writer order session→recheck→`+`→live-ref; session-held-until-folded; GC seal→§6.2 re-check→delete; **`gc_lock` dropped (G1)**; safety = the §7 proof + sessions-until-folded + fence lease; Keeper acceleration optional (1-PUT floor, tests pass with+without). Note the data-loss-risk / attended-review / do-not-merge-until-proven flag. Reference the spec + this plan.
- [ ] **Step 5: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- The writer follows the exact §7.1 order: **session (the flag) → upload → recheck-tomb → `+` (async) → live-ref → keep-session-until-folded → delete session**. Unlink removes the ref before the `-` (bias to over-count).
- Sessions live until **folded** (folded watermark), so *sessions ∪ folded-snapshot* always covers every live reference; the reaper is timer-safe (§7.3 mechanical rule, never a bare timer on a committed-but-unfolded session; no C++ `sleep`).
- The in-process `gc_lock` between commit and sweep is **dropped (G1)**; the sole delete gate is seal → grace → **§6.2 fresh authoritative re-check** → delete iff none AND tomb intact AND fence still mine.
- The §7 interleaving oracle and the §5.1 append-as-epoch-folds oracle pass exhaustively (no sleeps), plus two-gen-recover, tombstoned-but-present read, dup-`+`-across-close, and C1/C3/C4/C7.
- Op-count budgets assert the S3-only floor (≤2 sequential control writes, 1 `LIST`/reused `H`, no per-blob `HEAD`/`GET active`) and, with the optional Keeper accelerator, the 1-PUT normal-mode floor.
- The optional Keeper acceleration (§12.2) is a pure bounded cache with the seal-to-Keeper-before-recheck coherence rule; **every test passes WITH and WITHOUT Keeper**; Keeper loss degrades to the S3-only floor, never to incorrectness.
- CA suite + all `ContentAddressed*` gtests green. The **DO-NOT-MERGE-UNTIL-PROVEN** attended-review gate is satisfied. Backlog notes S4 done + the data-loss-risk / attended-review flag.
