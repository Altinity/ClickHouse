# Work log — CA GC attempt-scoped generation (concurrent-leader-leak fix)

Spec: `docs/superpowers/specs/2026-06-28-cas-gc-attempt-scoped-generation-design.md`
Branch: `cas-gc-part-manifest-impl`
Mode: unattended; superpowers ritual (writing-plans → review → subagent-driven TDD with per-task spec+quality review).

## Timeline

### 2026-06-28 — kickoff
- Spec approved by user. Read spec (381 lines). Goal property: *no unadopted artifact may ever influence a retire/delete/trim/fold decision; reader-visible generation state is exactly `(snap_generation, snap_attempt)`*.
- TLA+ Gate A (`INV_ONLY_ADOPTED_VIEWABLE` + R0 + `SabotageDeposedLeaderWritesFinalGen` negative control) must pass before any code task.
- Setting up worklog; invoking `writing-plans` to produce the implementation plan.

### writing-plans — ground-truth gathering
Dispatched 4 parallel exploration agents to extract verbatim code/signatures/line-numbers (a no-placeholder plan needs real code):
- TLA+ model `CaGcRootLocalPartManifestCore.tla` + cfgs + TLC runner (vars/actions/invariants/sabotage mechanism/dimension flags/state counts).
- `CasGc.cpp` GC functions (`runRegularRound`/`fold`/`recheck`/`retire`/`fence`/`tryResumeIncompleteRound`/`acquireOrRenewLease`/`zeroInDegree`/`previewDeletes`/`trim`/`readSealedCursors`/pruning + the lease-guarded `gc/state` CAS pattern).
- `CasLayout.h` key derivation + `CasGcFormats.{h,cpp}` GcState struct + Proto + `CasBlobInDegree.cpp` `foldDeltasIntoGeneration`.
- gtest build/run + existing GC gtests (`gtest_cas_gc_leak/fold/resume.cpp`, `cas_test_helpers.h`) + `CasInMemoryBackend` + S33 scenario card.

Relevant file map confirmed: TLA+ sources live in `docs/superpowers/models/` (40+ cfgs); CA GC Core in `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`; cas gtests in `src/Disks/tests/gtest_cas_*.cpp`.

**Agent 1 (CasLayout/Formats/BlobInDegree) — DONE. Key code-reality findings that shape the plan:**
- `Backend::putIfAbsent` returns `PutResult{outcome, token}` where `PutOutcome` is **two-state**: `Done` | `PreconditionFailed`. There is **no** byte-divergence outcome. ⟹ detecting "divergent bytes" for the strict deterministic-artifact rule requires a follow-up `get(key)` + `cityHash128` compare against the local bytes. `casPut(key, bytes, std::optional<Token> expected)` → `CasResult{CasOutcome: Committed|Conflict, token}`; `expected==nullopt` ⇒ create-if-absent.
- Key derivation (`CasLayout.h`, `class Layout`, member `String prefix`):
  - `gcStateKey()` → `<p>/gc/state`; `gcHbKey()` → `<p>/gc/hb`
  - `foldSealKey(gen)` → `<p>/gc/gen/<gen>/fold_seal`
  - `completionSealKey(gen)` → `<p>/gc/gen/<gen>/completion_seal`
  - `blobTargetRunKey(gen, shard, seq)` → `<p>/gc/gen/<gen>/blob_target/<shard>/<seq>` (callers pass seq=0)
  - `partManifestCleanupKey(gen, owner_shard, seq)` → `<p>/gc/gen/<gen>/part_manifest_cleanup/<owner_shard>/<seq>`
  - `retiredKey(round, fence_seq, shard)` → `<p>/gc/retired/<round>.<fence_seq>/<shard>`  ← **already epoch-keyed by fence_seq, NOT under gc/gen**
  - `outcomesKey(round, fence_seq, shard)` → `<p>/gc/outcomes/<round>.<fence_seq>/<shard>`  ← **already epoch-keyed**
  - **No `attempt` key/field exists anywhere yet.**
- **Plan adaptation (faithful to the spec's governing invariant, reconciled to code):** the wedge + divergent-run failures live exclusively in the four **`gc/gen/<gen>/…`** write-once artifacts (fold_seal, completion_seal, blob_target run, part_manifest_cleanup) — these key only off `generation`, so a deposed leader collides on the final key. `retired`/`outcomes` already carry `(round, fence_seq)` epoch separation (fence_seq bumps on lease steal) AND are written in the lease-guarded tail. ⟹ attempt-scope the **four gc/gen keys** (add an `attempt` param); document why retired/outcomes are already epoch-safe and need no new attempt segment. This is the minimal correct realization of "no unadopted artifact influences a decision."
- `GcState` struct (`CasGcFormats.h:45-54`): `uint64_t round, fence_seq, gc_shards, snap_generation, snap_pruned_through; GcLease lease; std::map<uint64_t,std::map<String,uint64_t>> fence_version;`. Add `uint64_t snap_attempt = 0;`. Proto `GcStateProto` (`Proto/cas_format.proto:149-158`) max field# = 8 ⟹ add `uint64 snap_attempt = 9;`. Encode at `CasGcFormats.cpp:78-82`, decode at `129-135`. Note proto `snap_shards`(=4) ↔ struct `gc_shards`.
- `foldDeltasIntoGeneration(Backend&, const Layout&, uint64_t prior_generation, uint64_t new_generation, uint64_t shard, std::vector<BlobDelta> scattered, std::vector<RunRef>& out_runs)` (`CasBlobInDegree.cpp:89`, `.h:34`). Offending lines 168-173: `backend.putIfAbsent(run_key, run_bytes)` return discarded; then `out_runs.push_back(RunRef{.key=run_key, .checksum=cityHash128(run_bytes)})`. `RunRef{String key; UInt128 checksum;}` (`CasGenerationSeal.h:15`). OQ5 comment at line 93. The run key `blobTargetRunKey(new_generation, shard, 0)` is built **inside** this function (and inside `ShardReducer::reduce`), so attempt-threading must reach here.

**Agent 2 (`CasGc.cpp`) — DONE. All members in `DB::Cas::Gc`. Verbatim landmarks:**
- `runRegularRound()` (71-144): `acquireOrRenewLease` → `tryResumeIncompleteRound` → `fold`(R1) → `retire`(R2) → `fence`(R3) → `recheck`(R4) → `trim`. state+token threaded by ref, never re-read mid-round (zombie-steal protection).
- `fold` (196-457): `new_generation = snap_generation+1` (217). fold_seal putIfAbsent + divergent-`get`-compare-throw at **438-448**; then `state.snap_generation = new_generation; casPut(gcStateKey, encodeGcState(state), state_token)` at **450-456** (this is the fold-adopt → becomes CAS #1).
- `recheck` (667-958): `completion_generation = snap_generation+1` (719); single content `deleteExact` at **779**; outcome-log putIfAbsent at **826-848** (already keyed `outcomesKey(round, fence_seq, shard)`); completion_seal putIfAbsent+divergent throw at **915-925**; the completion-advance `casPut` (`next.snap_generation = completion_generation`) at **927-944** (→ CAS #2); retired-set drop at 946-957.
- `retire` (459-577): `round = state.round+1`; candidates from `zeroInDegree(backend, layout, folded.fold_seal.generation, 0)`; writes `retiredKey(round, fence_seq, shard)` at 545-565 (**already fence_seq-keyed**); `.round`-advance casPut at 567-575.
- `fence` (579-665): does **not** touch fence_seq; advances `fence_version[round]` per shard via one gc/state CAS (636-664).
- `tryResumeIncompleteRound` (1345-1414): incompleteness signal = retired sets still present at `(round, fence_seq)`; rebuilds fold tail from `readFoldSeal(snap_generation)`; re-fences if `!fence_version.contains(round)`; re-runs `recheck`+`trim`.
- `acquireOrRenewLease` (1612-1698): returns GcState+state_token by ref. **Steal path 1679-1690**: `++next.lease.seq; ++next.fence_seq; casPut(...)`. **Steal is the ONLY `++fence_seq` site.** Renew keeps lease.seq. ⟹ `lease.seq` = (#creates+#steals), unique per leader tenure → the right **attempt** discriminator for the gc/gen keys.
- Pruning = `pruneSupersededGenerations(adopted_generation, next)` (1068-1113): retention `keep = gc_snap_generations_to_keep`; `prune_floor = adopted - keep`; bounded `kMaxPrunePerRound=64`; drops foldSeal/completionSeal/blobTargetRun/partManifestCleanup per generation via `dropExact`; advances `snap_pruned_through`. Called from recheck at 937 (folds into CAS #2).
- Canonical lease-guarded CAS (retire 567-575): `next=state; mutate; casPut(gcStateKey, encodeGcState(next), state_token); if(!Committed) throw ABORTED; state=next; state_token=res.token;`.

**Agent 3 (gtest/build/S33) — DONE:**
- Build: `ninja unit_tests_dbms` (target `src/CMakeLists.txt:886`). Binary `build/src/unit_tests_dbms` (built 2026-06-28). Run `./unit_tests_dbms --gtest_filter='CasGc*'`. **No CMake edit needed for new gtests** — `grep_gtest_sources` GLOB_RECURSE `gtest*.cpp` over `src/` with `CONFIGURE_DEPENDS` auto-picks new files; CAS core already linked via `dbms`.
- Existing GC gtests in `src/Disks/tests/`: `gtest_cas_gc_fold.cpp` (`TEST(CasGcFold,...)`), `gtest_cas_gc_resume.cpp` (`TEST(CasGcRound/Resume,...)`), `gtest_cas_gc_leak.cpp` (`TEST(CasGcLeak,...)` + `CasReuseGcRace`). Pattern: `auto backend = std::make_shared<InMemoryBackend>(); auto store = openStoreForTest(backend); Gc gc(store, kGc=hexToU128("..01")); gc.runRegularRound();` then assert via `inDegreeOf`, `runFsck`, `backend->head(...)`. Helpers in `cas_test_helpers.h` (namespace `DB::Cas::tests`).
- `InMemoryBackend` (not final; tests subclass). **Fault-injection levers for an in-process two-leader collision gtest (no real threads):** `failNextCasPut(key)`, `setHoldDeletes/pendingDeletes/landPendingDelete`, `setEnforceTokens`. This is how a new `gtest_cas_gc_attempt.cpp` can drive A-folds → B-steals (mutate gc/state lease) → A's adopt CAS conflicts → assert A's attempt invisible.
- **S33** (`utils/ca-soak/scenarios/cards/s28_s33_corner.py:733-928`): soak regression guard for `GC-CONCURRENT-LEADER-LEAK`. Fires `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION ca` on both nodes via `threading.Barrier(2)`. Asserts SAFETY `fsck dangling==0` (always) + LIVENESS forced GC drains `unreachable→0` (currently EXPECTED FAIL = the leak). Fix must flip LIVENESS green while keeping SAFETY.

**Reconciliation note for the plan:** retired/outcomes are already `fence_seq`-epoch-separated; only the four `gc/gen/<gen>/…` write-once keys collide. Minimal fix = thread an `attempt` (= `lease.seq`) into `foldSealKey/completionSealKey/blobTargetRunKey/partManifestCleanupKey`, store `snap_attempt` in gc/state, resolve all gc/gen reads via `(snap_generation, snap_attempt)`. A deposed leader's orphaned retired/outcomes (under stale fence_seq) is pre-existing bounded space debris — note as minor, covered by the bounded sweep, not a correctness gap.

**Agent 4 (TLA+) — DONE. PIVOTAL finding → TLA+ gate modeling decision.**
- `CaGcRootLocalPartManifestCore.tla` (1203 lines, runner `run_gc_partmanifest.sh <cfg-basename>`, jar at `tmp/tla2tools.jar`, logs to `tmp/tlc_<cfg>.log`) has **no lease/`lease.seq`/`fence_seq`/`snap_generation`/`gc/state` record and no lease-steal action**. Leader competition modeled via `gcPhase`/`roundOf` + `reducerOwner`/`DisjointShardOwnership` single-coordinator partition (Phase 4 sharding). Generation artifacts = the per-round maps `foldSeal[r]={classified, foldedCursor}` and `completionSeal[r]={fenced, rechecked, deleted, adoptable}`; `adoptable` is the round-visibility flag (set by `GCoordSeal`:957). No per-attempt seal slot exists.
- The literal stealable lease lives in **`CaGcLeaseCore.tla`** (`stOwner/stSeq/stFence`, observation-window steal, `EnableHeartbeat` dimension). `RetireCommit` (131-133) proves a **displaced leader never commits** (`stFence=roundFence[a]` else wasted round) — the zombie-steal protection = the code's token-gated gc/state CAS. But it models no seals/blobs.
- **DECISION (documented deviation-by-reconciliation from the spec's literal wording, faithful to its intent):** the gate is proven by **extending `CaGcRootLocalPartManifestCore.tla`**, where the spec's "two leaders steal the lease / a deposed leader writes a final-gen artifact" maps to this model's vocabulary as: a **second leader writes a generation seal for a round/generation it does not own** (a new flag-gated `GDeposedWriteSeal`-style action), and a **generation-pointer/adopt** records which attempt is viewable. The lease-steal *safety half* is already proven in `CaGcLeaseCore` — this model proves the orthogonal **artifact-visibility half**. "attempt" in TLA+ = the writing leader's identity (the model already has `Leaders`).
- New constant `EnableAttemptScoping` threaded like `EnableSharding` (gates action arms in `Next` + invariant via `Enable* => P` vacuity + new vars inert when FALSE, following the `coordFence/shardIndeg/storedTok` pattern). New constant `SabotageDeposedLeaderWritesFinalGen` woven as `IF flag THEN <unsafe shared-slot write> ELSE <honest>` per the existing sabotage idiom.
- **Inertness target (cheap, load-bearing):** with `EnableAttemptScoping=FALSE`, `stage0` reproduces EXACTLY **71,184 / 19,846** and `stage1` EXACTLY **1,659,466 / 402,034** (per `CaGcRootLocalPartManifestCore_RESULTS.md`). Cost note: stage3 (27m) / stage5_sharding (65m) need re-running only if a *shared, ungated* action changes; perfect gating ⟹ stage0+stage1 reproduction is the established sufficiency bar.
- Mechanics: **every** one of the ~38 `CaGcRootLocalPartManifestCore_*.cfg` enumerates all constants ⟹ both new constants must be bound (`FALSE`) in all of them; add 2 new cfgs (`_stageN_attemptscoping` positive with `EnableAttemptScoping=TRUE` + `Leaders={L1,L2}`, and `_sab_deposedleaderwritesfinalgen`).
- Existing R0 oracle preserved (the real safety): `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE` (verbatim defs captured). New safety invariant `INV_ONLY_ADOPTED_VIEWABLE`. Sabotage `SabotageDeposedLeaderWritesFinalGen` must produce a counterexample (R0 or INV_ONLY_ADOPTED_VIEWABLE).

All 4 agents complete. Writing the plan.
