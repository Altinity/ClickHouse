# Codex-Review Triage Fix Wave Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land every confirmed REAL-FIX from the codex-review triage (`docs/superpowers/reports/2026-07-17-codex-review-triage.md` §6), plus the by-design comment wave and BACKLOG defers — excluding the §4 design-decision items (№14/15/17/29/30/31), which the user postponed.

**Architecture:** Eleven independent fixes, each following the remediation direction and file:line evidence recorded in the triage report (the report is the spec; each task names its section). Fail-closed throughout: a fix must never trade a confirmed hazard for silent degradation.

**Tech Stack:** C++ (ClickHouse), gtest (`src/Disks/tests/`), TLA+ (Task 1 only), branch `cas-gc-rebuild`.

## Global Constraints

- Branch `cas-gc-rebuild`; new commits only — NO rebase, NO amend, **NO PUSH** (mandate revoked by the user).
- Allman braces; comments state constraints, not narration; never `sleep` to fix a race.
- Fail-closed: on error prefer propagation over silent defaults; no destructive action on a fallback path.
- The spec for every task = `docs/superpowers/reports/2026-07-17-codex-review-triage.md` (cite its section per task below). Verifier file:line references are from HEAD `3ada085f549`; re-verify line numbers before editing.
- Build: `ninja -C build <target> > build/<log> 2>&1` (never `-j`, never `nproc`); analyze logs via subagent.
- Gtest battery filter (run after EVERY code task, expect 0 failures):
  `build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'` (current baseline: 907/907).
- New tests: prefer a new gtest in the existing `src/Disks/tests/gtest_cas_*.cpp` file that owns the subsystem; TDD (write the failing test first) wherever the harness allows deterministic reproduction.
- When a task's edit contradicts something discovered in the code, STOP and report BLOCKED — do not improvise around the spec.

---

### Task 1: №4 — condemn marker becomes load-bearing (GC delete gate) {#t1}

Spec: triage §3.4. THE top-severity item: a swallowed `writeCondemnedMeta` lets a writer
adopt the same token that a later `deleteExact` kills (dangling manifest); rebuild writes
no markers at all.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp`
  (condemn sites ~:876 and ~:513; `scheduleMetaJob` ~:183; graduation/settle path via
  `CasBlobInDegree.cpp:401-427`; rebuild `~:2043-2062`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp` (settleEntry / graduation)
- Test: the gtest file owning GC round logic (`git grep -l "settleEntry\|delete_pending" src/Disks/tests/`)
- TLA+: the writer↔GC model that owns condemn/adopt (`find tla/ docs -name '*.tla' | xargs grep -l -i condemn`)

**Interfaces:**
- Consumes: `writeCondemnedMeta(hash, token)` (existing), retired-entry records `(hash, token)` in fold seal runs.
- Produces: a per-entry durable-marker confirmation bit consulted at graduation; entries without it are CARRIED, not deleted.

**Design decision recorded in the triage (verifier option (a))**: keep GC's async/advisory
meta model everywhere EXCEPT the one edge that authorizes an irreversible delete:
graduation to `delete_pending` (or the redelete itself) requires a CONFIRMED durable
`Condemned` meta for the exact `(hash, token)`. "Confirmed" = the marker write completed
successfully in THIS process during this or an earlier round (tracked on the retired
entry), OR a fresh `loadMeta(hash)` read observes `state == Condemned && token == entry.token`.
On absence → carry the entry to the next round (fail-safe delay, never fail-open delete).
The standing GC rule is untouched: never throw on a 404 during fold — this gates a DELETE
on missing evidence; it does not throw.

- [ ] **Step 1: Read the four sites end-to-end** (`scheduleMetaJob`, both condemn sites, `settleEntry`/graduation in `CasBlobInDegree.cpp:401-427`, rebuild `zero_condemned` block) and write down: where the retired entry is created, what fields it carries through fold-seal encode/decode (`CasFoldSealFormat`), and where graduation decides `delete_pending`.
- [ ] **Step 2: Write the failing gtest** — deterministic shape: condemn a blob with the marker write FAILING (test backend fault on the meta key), advance rounds to graduation, assert the entry is CARRIED (blob still present, entry still retired) instead of deleted. Second test: marker write succeeds → graduation deletes as today. Third test: rebuild path — after rebuild, entries must have markers (assert marker object exists) before any graduation.
- [ ] **Step 3: Run the new tests, verify they FAIL on current code** (first and third).
- [ ] **Step 4: Implement.** (a) At both condemn sites: record marker-write success on the retired entry (the `scheduleMetaJob` completion callback sets it; a swallow leaves it unset). (b) If the retired-entry format lacks a field for this, add `marker_confirmed` (bool) to the retired-run record in `CasFoldSealFormat`/record-stream codec — pre-release, no compat scaffolding needed (project rule). (c) At graduation/settle: entry without confirmation → attempt ONE synchronous `loadMeta` re-check (`Condemned` + token match ⇒ confirm); still unconfirmed → carry (keep in retired, skip delete this round) + `ProfileEvents::increment` a new `CasGcCondemnMarkerUnconfirmedCarry` counter. (d) Rebuild: publish `writeCondemnedMeta(hash, token)` for every `zero_condemned` entry SYNCHRONOUSLY (rebuild is already an offline/administrative path); entries whose marker write fails enter the retired set unconfirmed (carried by (c)).
- [ ] **Step 5: TLA+ gate.** Extend the writer↔GC condemn model: add the `marker write may fail` transition and the graduation guard; run TLC; the pre-fix model must show the counterexample (writer adopts same token, delete fires), the post-fix model must pass. If no existing model covers condemn/adopt, write a minimal new one (states: blob token, meta state, writer edge, retired entry ± confirmation; invariant: no delete of a token with a live edge).
- [ ] **Step 6: Run the new gtests → PASS; run the full Ca* battery → 0 failures.**
- [ ] **Step 7: Commit** `cas: gc — condemn marker is load-bearing: graduation gated on confirmed durable meta (triage #4)`.

### Task 2: 19c — emu tokens seeded from etag (+ №18 collapse, №19 type check) {#t2}

Spec: triage §3.18. Latent local-CA data loss across restart: `emu_seq` counter re-mints
values that can collide with persisted condemn tokens. The fix implements what the
comments already claim: etag(mtime-ns) seeding.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp` (`emuObserveToken` :531-542; `emuWrite`; list branch :935-955; conditional ops :716/:754/:776/:816)
- Modify: `.h` (comment at :58-61 stays truthful once code matches)
- Test: `src/Disks/tests/` file owning backend emu tests (`git grep -l emuObserveToken src/Disks/tests/` or the CountingBackend/backend gtest)

**Interfaces:**
- Produces: emu token VALUE = the underlying object's etag (LocalObjectStorage mtime-ns), type stays `TokenType::Emulated`; `list` surfaces the same value under the same type.

- [ ] **Step 1: Write the failing gtest** — (a) restart-collision shape: create object, observe token T1, simulate process restart (new `ObjectStorageBackend` instance over the same storage), delete+recreate the object (fresh mtime), assert `deleteExact(key, T1)` returns `TokenMismatch` (fails today when the fresh counter re-mints T1's value); (b) list/head agreement: `list` token == `head` token for the same key (fails today: ETag-type vs Emulated-type).
- [ ] **Step 2: Run → FAIL (both).**
- [ ] **Step 3: Implement.** `emuObserveToken`/`emuWrite`: token value = the object's current etag from the object storage metadata (for LocalObjectStorage this is mtime-ns; fall back to a fresh monotonic value ONLY if the storage returns an empty etag — and then persist nothing). Keep `TokenType::Emulated`. Drop the `emu_seq`/`emu_tokens` counter map where the etag makes it redundant (keep the map only if in-process mutation ordering needs it — decide from the code; the invariant is: same bytes+mtime ⇒ same token, new incarnation ⇒ new token). `list` branch: return `Token{etag, TokenType::Emulated}` under the emu path (do NOT call `tokenForList`, whose type is native). №19 hardening: in `putOverwrite`/`casPut`/`deleteExact` (both modes), reject `expected.type` mismatching the backend's minting type with `PreconditionFailed` (never send a foreign-dialect value to the wire).
- [ ] **Step 4: mtime-resolution check.** Two writes within one mtime quantum must not mint identical tokens for DIFFERENT incarnations: verify LocalObjectStorage etag is nanosecond mtime; if the filesystem truncates to seconds (CI ext4 is ns — but verify), mix in file size or an in-process nonce for same-mtime rewrites. Document the chosen rule at `emuObserveToken`.
- [ ] **Step 5: Run new tests → PASS; full Ca* battery → 0 failures; run one local-CA scenario smoke (`python3 -m scenarios.run --scenario S02 --scale dev --seed 1` from `utils/ca-soak`) → PASS.**
- [ ] **Step 6: Commit** `cas: emu backend — etag-seeded tokens (closes triage 19c restart collision + #18 list dialect; #19 type check)`.

### Task 3: №5 — generation prune protects parent ∪ proposed {#t3}

Spec: triage §3.5.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:570-573`
- Test: GC round gtest file (same as Task 1's).

- [ ] **Step 1: Write the failing gtest** — construct: parent seal referencing run at generation g_old for shard s; proposed (folded) seal referencing g_new for s; call the prune with prune floor covering g_old; assert the g_old run object SURVIVES. (Today it is deleted.)
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement** — where `referenced_generations` is built (`CasGc.cpp:570-572` iterating `folded.fold_seal.blob_target_runs`), also insert every generation from `parent_seal_runs` (captured at :341-343). Add the constraint comment: "pre-CAS prune may only delete generations neither the PROPOSED nor the PARENT seal references — a losing leader must not destroy what the winning leader's seal still points at (triage #5)."
- [ ] **Step 4: Run new test → PASS; Ca* battery → 0 failures.**
- [ ] **Step 5: Commit** `cas: gc — pre-CAS generation prune protects parent ∪ proposed seal references (triage #5)`.

### Task 4: №22 — GC config bounds fail closed {#t4}

Spec: triage §3.22.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp` (~:247 `gc_interval_sec`, ~:272 `gc_shards`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.cpp:22` (chassert → throw)
- Test: format gtest (`git grep -l decodeGcState src/Disks/tests/`)

- [ ] **Step 1: Failing test** — `encodeGcState` with `gc_shards=0` must THROW `LOGICAL_ERROR` (today: chassert, release-inert → encodes).
- [ ] **Step 2: Run → FAIL (in a RelWithDebInfo build the chassert is compiled out, so the current code encodes silently).**
- [ ] **Step 3: Implement** — (a) factory: after reading the two values, `if (gc_interval_sec == 0 || gc_shards == 0) throw Exception(ErrorCodes::BAD_ARGUMENTS, "content_addressed disk '{}': gc_interval_sec and gc_shards must be >= 1 (got {}, {})", ...)`. (b) `encodeGcState`: replace `chassert(state.gc_shards >= 1)` with `if (state.gc_shards < 1) throw Exception(ErrorCodes::LOGICAL_ERROR, "encodeGcState: gc_shards must be >= 1 — refusing to persist an unreadable gc/state");`.
- [ ] **Step 4: Run tests → PASS; Ca* battery → 0 failures.**
- [ ] **Step 5: Commit** `cas: fail closed on zero gc_interval_sec/gc_shards before pool open (triage #22)`.

### Task 5: №9 — decommission tail fenced + DeleteOutcome inspected {#t5}

Spec: triage §3.9. The reset→get gap lets a returning victim's fresh control objects be
exact-deleted; epoch monotonicity is the crown jewel at stake.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp:148-183`
- Test: decommission gtest (`git grep -l decommissionPoolMember src/Disks/tests/`)

**Fix contract (from the verifier, refined):**
1. CAPTURE while the claim is held: the epoch object token+value observed under the claim, and the farewell mount token that `finishTeardown` itself writes (obtainable by a `get(mountKey)` immediately after `admin.reset()` — but see 3).
2. Every slot delete inspects its `DeleteOutcome`; any non-`Deleted` outcome → abort the tail, report `slot_removed=0` with the reason (fail closed; the slot stays for the live successor).
3. Ordering that removes the re-read window: delete `mount` FIRST using the exact farewell token (a successor reclaim rewrites the mount → `TokenMismatch` → abort); only after `mount` is provably gone delete `epoch` with the token captured UNDER the claim (a successor bumps epoch → mismatch → abort); delete `owner` LAST (same-uuid successors never rewrite owner, so owner may only be deleted once mount+epoch deletions PROVED no successor exists).

- [ ] **Step 1: Failing gtest** — interleaving harness: run `decommissionPoolMember` against a test backend where, between the admin release and the slot deletes, a "successor" rewrites mount+epoch (fresh tokens). Assert: no successor object is deleted, the command reports failure/slot-retained. (Today: successor's objects are deleted, `slot_removed=1`.) Second test: no successor → tail deletes all three, `slot_removed=1`.
- [ ] **Step 2: Run → FAIL (first).**
- [ ] **Step 3: Implement per the contract above.** Reuse the existing `DeleteOutcome` classification used by `deleteListedPrefix` in the same file.
- [ ] **Step 4: Run tests → PASS; Ca* battery → 0 failures.**
- [ ] **Step 5: Commit** `cas: decommission — fence slot deletes against successor reclaim, inspect every DeleteOutcome (triage #9)`.

### Task 6: №7 — receiver pool-UUID recheck → byte fallback {#t6}

Spec: triage §3.7.

**Files:**
- Modify: `src/Storages/MergeTree/DataPartsExchange.cpp` (after `disk = reservation->getDisk()` ~:689, before the relink commit ~:752)
- Test: integration-level; minimal deterministic coverage = a unit-less code-path guard is impractical here, so the test is the S38-style integration check. For THIS task: add the guard + a `LOG_INFO` on fallback; validate by (a) compiling, (b) the existing stateless/battery staying green, (c) grep-level assertion in review. Flag in the report that runtime coverage lands with the R5 campaign (S38 exercises fetch).

- [ ] **Step 1: Read the fetch flow** (`:544-561` advertise, `:625-691` reserve, `:730` `fall_back_to_byte_fetch`, `:752-757` commit) and identify the advertised-pool variable's name/scope at the commit site (it may need plumbing from the request into the commit lambda).
- [ ] **Step 2: Implement** — at the point the relink payload is about to be consumed: `const auto chosen_ca = tryGetContentAddressedExchange(disk); if (!chosen_ca || chosen_ca->getPoolUUID() != advertised_pool_uuid) return fall_back_to_byte_fetch("reservation landed outside the advertised pool");` (exact call shapes from the surrounding code; the byte-fallback lambda already exists at :730). BOTH mismatch cases (non-CA disk, different pool) route to bytes — no throw.
- [ ] **Step 3: Build (`ninja -C build clickhouse` → log → subagent) + Ca* battery → 0 failures.**
- [ ] **Step 4: Commit** `cas: fetch — re-check pool uuid after reservation, byte-fallback on mismatch (triage #7)`.

### Task 7: №16 — `file_view` joins the ReaderExecutor fallback condition {#t7}

Spec: triage §2.16.

**Files:**
- Modify: `src/IO/ReadPipeline.cpp:208-217`
- Test: none runnable without `use_reader_executor` infrastructure; the change is a one-line fail-closed guard.

- [ ] **Step 1: Implement** — extend the condition:
```cpp
    if (distributed_cache || memory_cache || !filesystem_caches.empty()
        || !decryption_stages.empty() || async_prefetch || file_view)
```
and the log message: `"(caches/decryption/file_view not yet supported by the executor)"`.
- [ ] **Step 2: Build + Ca* battery → 0 failures.**
- [ ] **Step 3: Commit** `io: reader executor falls back when a file_view byte window is configured (triage #16)`.

### Task 8: №1 — no unconditional fallback under a requested precondition {#t8}

Spec: triage §3.1.

**Files:**
- Modify: `src/IO/S3/copyS3File.cpp` (`processCopyRequest` :742-758, `performMultipartUploadCopy` :808-815)
- Test: `src/IO/tests/` or the existing copyS3File unit context if present; otherwise assert via code-shape + battery (S3 fault injection for AccessDenied-mid-copy has no harness — note it in the report).

- [ ] **Step 1: Implement** — in BOTH fallback branches, before invoking `fallback_method()`:
```cpp
    if (request_settings.if_none_match.has_value())
        throw ...; /// re-throw the original error: a conditional copy must fail closed,
                   /// never degrade to an unconditional read/write copy (CAS write-once pool)
```
(Exact member spelling from the file — the conditional rides `if_none_match` per :211/:706; rethrow the caught S3 error rather than minting a new one.)
- [ ] **Step 2: Grep-audit**: no other `fallback_method` invocation exists on a conditional path (`grep -n fallback_method src/IO/S3/copyS3File.cpp`).
- [ ] **Step 3: Build + Ca* battery + one CA-S3 gtest-level probe run if available.**
- [ ] **Step 4: Commit** `s3: conditional copy never falls back to an unconditional write (triage #1)`.

### Task 9: №8 + №10 — lifecycle/TSan pair {#t9}

Spec: triage §3.8, §3.10. Do both in one task (both are Pool/metadata lifecycle, both TSan-relevant pre-R6).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (`runGarbageCollectionRoundNow` :359-368, `runOneGcRoundForTest` :228-237, `shutdown` :541-543, lazy creation site, `store()` :549, `partAccess()` :557)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.{h,cpp}` + the `Pool::open` call site (:472) for the sink.
- Test: existing lifecycle gtests; the UAF itself is only TSan-visible — the deliverable is shape correctness + battery green; R6 validates.

- [ ] **Step 1 (№8): Implement** — (a) `runGarbageCollectionRoundNow`/`runOneGcRoundForTest`: hold `gc_scheduler_mutex` across the whole `runOneRoundNow()` call (mirror `gcHealth`; the round is long — add a comment accepting that a concurrent shutdown waits, which is the CORRECT priority). (b) Add `bool shutdown_called TSA_GUARDED_BY(gc_scheduler_mutex)`; `shutdown()` sets it under the lock before reset; the lazy-creation path throws/returns null when set. (c) `cas_store`/`part_access`: guard reads and shutdown-resets with one lifecycle mutex OR switch accessors to return by-value `shared_ptr` snapshots taken under it (choose whichever touches fewer callers; `store()` currently returns `const PoolPtr&` — by-value return is the safer contract).
- [ ] **Step 2 (№10): Implement** — thread `CasEventSink` into pool construction: add `PoolConfig::event_sink` (or a `Pool::open` parameter), assign `event_sink_` BEFORE `mountWritable` spawns the renewal thread (`CasPool.cpp:499`); `ContentAddressedMetadataStorage.cpp:472-483` passes `makeCasEventSink()` at open instead of calling `setEventSink` after. Keep `setEventSink` for tests but document it as pre-open-only.
- [ ] **Step 3: Build + full Ca* battery → 0 failures.**
- [ ] **Step 4: Commit** `cas: lifecycle — scheduler round holds its mutex, no post-shutdown creation, event sink installed before threads (triage #8, #10)`.

### Task 10: contract batch (№12n, №13, №20a, №20c, №21, №23, №24, №25, №28) {#t10}

Spec: triage §2.12/§2.13/§3.20/§2.21/§2.23/§2.24/§2.25/§2.28. Nine independent SMALL fixes;
one commit each (reviewable one-by-one), one implementer.

- [ ] **№12-narrow** (`ContentAddressedTransaction::removeDirectory`): also `st->entries.clear();` and if `st->build` → `st->build->abandon(); st->build.reset();` when dropping a part ref, so a same-txn create-then-remove publishes nothing. Gtest: stage entries for a ref, `removeDirectory`, `commit`, assert ref absent.
- [ ] **№13** (`CasPartWriteTxn` promote emission ~:1068, second site ~:926, `ContentAddressedTransaction::commit` emission if any): wrap post-durable `EventEmitter{...}.emit(...)` in `try { ... } catch (...) { tryLogCurrentException(log, "CAS event emission after durable publish"); }`. Do NOT touch pre-durable emissions.
- [ ] **№20a** (`CasServerRootFormats.cpp` `decodeMountLease`, `CasGcStateFormat.cpp` `decodeGcHeartbeat`): track `saw_su/saw_we` (resp. `saw_by/saw_seq`) and throw `CORRUPTED_DATA "mount-lease: missing identity field"` when absent — mirroring `decodeOwner`'s existing shape. Gtest per decoder: body without identity → throws.
- [ ] **№20c** (`CasGc.cpp` `readFoldSeal` :1692 + `decodeFoldSeal`): pass the requested `generation` in; after decode, `if (seal.generation != expected_generation) throw CORRUPTED_DATA` (mirror `decodeRefTableSnapshot:327`). Gtest: seal encoded for g=5 read via key g=6 → throws.
- [ ] **№21** (`CasTextFormat.cpp`): add `uint32_t JsonObjectReader::readU32Number()` = `readU64Number()` + `if (v > std::numeric_limits<uint32_t>::max()) throw CORRUPTED_DATA "value out of uint32 range"`; use it at the three header-version sites (`CasTextFormat.cpp:292`, `CasRecordStreamFormat.cpp:134`, `CasBlobEnvelopeFormat.cpp:189`). Gtest: header with `"v":4294967299` → CORRUPTED_DATA (today: passes as 3).
- [ ] **№23** (`ContentAddressedTransaction::truncateFile` :1382): replace the no-op with `throw Exception(ErrorCodes::NOT_IMPLEMENTED, "truncateFile is not supported on a content-addressed disk (blobs are immutable; whole-file rewrites replace the staged entry)")`.
- [ ] **№24** (`ContentAddressedTransaction::unlinkFile` :1333): honor `if_exists` — for the part-file route, when NOT staged here AND the committed manifest lacks the path (check via the same `getView` used by `publishStaging`) AND `!if_exists` → throw `FILE_DOESNT_EXIST`; same existence contract for the verbatim/mountpoint branches (their remove primitives' outcomes tell absence). Gtest: unlink of a nonexistent committed file without `if_exists` → throws; with → no-op.
- [ ] **№25** (`ContentAddressedTransaction::commit` catch block): add member `bool failed = false;` set in the catch before rethrow; `commit`/`tryCommit` start with `if (failed) throw LOGICAL_ERROR "retrying a failed content-addressed transaction is not supported"`. Gtest optional (constructor-level shape).
- [ ] **№28** (`Pool::beginPartWrite` :761-777): scope guard — `SCOPE_EXIT` variant that retires the allocated seq unless dismissed after `registerInflightBuild` succeeds (match the existing retire call `retireBuildSeq`).
- [ ] After all nine: build + full Ca* battery → 0 failures; nine commits with messages naming the triage numbers.

### Task 11: comment wave + BACKLOG defers {#t11}

Spec: triage §5, §6 DEFER list. No behavior changes.

- [ ] **Comments** (each a short constraint statement, not narration):
  - `resurrectStaged` (`CasObjectStorageBackend.cpp:~887`): the §3.2 by-design rationale (content-addressed byte-identity; tokenless-on-ref promote; fresh-tag defeats stale deletes).
  - `ObjectStorageBackend::get` (:548): the §3.3 ordering argument (token never newer than bytes; conditional consumers fail closed in exactly the mixed case).
  - `ContentAddressedTransaction::removeDirectory`/`moveDirectory`: call-time durability + compensation contract (§2.12).
  - `casPutObject`/append path (`CasPlainObjects.cpp` + `ContentAddressedTransaction.cpp:694`): the single-appender invariant (§3.26) — "correct only while nothing concurrently appends to one key; implement casAppendObject (re-read base in the loop) before adding a concurrent appender".
  - `moveFile` re-drive branch: one added line naming the reviewer-confusion ("an unrelated pre-existing destination has no producer under the single-writer contract — dst names derive from src").
- [ ] **BACKLOG entries** (`docs/superpowers/cas/BACKLOG.md`): (a) №6 — wire the fetch-handoff pin per spec `2026-07-15-cas-fetch-handoff-retention-pin-design.md` (triage re-confirmed the gap; DEFER, own task); (b) №11 — GC backstop for empty ownerless Live namespaces (verifier's remediation, LOW); (c) №26 — `casAppendObject` before any concurrent appender.
- [ ] Build (comment-only still must compile — `-Wdocumentation` traps per memory), Ca* battery, commit `cas: triage comment wave + backlog defers (#2,#3,#12,#26,#27 comments; #6,#11,#26 deferred)`.

---

## Self-Review Notes {#self-review}

- Spec coverage: §6 items 1-10 → Tasks 1-10; comment wave + defers → Task 11. §4 items deliberately absent (user postponement). №18 folded into Task 2 (root fix collapses it) — the list-branch change is explicit there.
- Line numbers are from the triage/verifier reports at `3ada085f549`; every task's Step 1 includes re-reading the site.
- Task 1 is the only task with real design freedom; its Step 4 fixes the decision (option (a)) so the implementer doesn't re-litigate.
- Test-first is specified everywhere a deterministic gtest can exist; Tasks 6/7/8 name their coverage limits explicitly rather than pretending.
