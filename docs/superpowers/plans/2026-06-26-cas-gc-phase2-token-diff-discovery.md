---
description: "Token-diff discovery and lazy body reads for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 2 (discovery)"
sidebar_position: 7
slug: /superpowers/plans/2026-06-26-cas-gc-phase2-token-diff-discovery
title: "Phase 2 — Token-Diff Discovery And Lazy Read — Implementation Plan"
doc_type: reference
---

# Phase 2 — Token-Diff Discovery And Lazy Read — Implementation Plan {#phase-2-token-diff-discovery-and-lazy-read-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Read `2026-06-26-cas-gc-redesign-overview.md` first, then this plan. **Gate:** this phase's TLA+ model extension (Task 1) must be GREEN before any code task (Tasks 2–6) starts. **Depends on:** Phase 1d (`2026-06-26-cas-gc-phase1d-gc-fold-indegree-sweep.md`).

**Goal:** Make GC `discover` skip an unchanged root shard's body read when the listed root token equals the persisted folded token, reading the body on any mismatch/missing/ambiguous/unsupported token, while never shrinking the registry namespace universe — proven safe by a Phase-0-model extension first.

**Architecture:** The `gc/registry` stays the authority for the namespace universe; LIST is only an accelerator. Phase 1d already persists per-root-shard coverage inside `ShardCoverage{classification, folded_token, folded_cursor}` of the `GenerationSeal`. Phase 2 adds (a) a backend capability probe `supportsListTokens` (the LIST seam may or may not surface per-key tokens), (b) round-trip persistence of the folded root-shard token + cursor in `ShardCoverage`, and (c) the token-diff rule in `discover`: a listed root token equal to the persisted folded token ⇒ skip the body read; token missing/ambiguous/stale/unsupported ⇒ read the body; LIST never shrinks the registry universe; fail closed to body reads on any ambiguity.

**Token-diff skips ONLY the body read / re-fold, never the fence.** A token-unchanged shard's elided work is exactly its `discover`/`fold` body read and re-fold — its blob in-degree and delete decisions are already covered by the persisted folded state. The all-shard fence from Phase 1d is **unchanged and orthogonal** to token-diff: every shard in the fence universe is still fenced every round (a publish into one shard can protect blobs in any target shard, spec §Global Fence). Phase 2 touches only `discover`; it must not let any task elide, defer, or reuse a fence. (Lazy *fencing* is a separate, later concern and is explicitly NOT in scope here.)

**Tech Stack:** TLA+ / TLC (`tla2tools.jar` at `tmp/tla2tools.jar`, OpenJDK 21) for the safety gate; C++ (ClickHouse coding standards, Allman braces) for the backend seam, `ShardCoverage` codec, and `CasGc` discovery; gtest (`unit_tests_dbms`, filter `Cas*:Ca*`) for unit oracles.

## Global Constraints {#global-constraints}

*Every task below implicitly includes this section. Copied verbatim from the redesign overview.*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in a phase may begin until that phase's TLA+ gate is green.
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
- Build into a `build_*` directory (e.g. `build`, `build_debug`, `build_asan`). Always redirect ninja output to `<build_dir>/build.log`. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `<build_dir>/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>[.sh]`. Do not add `no-*` tags unless strictly necessary. Prefer a new test over extending an existing one.
- Run CA gtests via the gtest binary built in the build dir with `--gtest_filter='Cas*:Ca*'`. The unit-test target is `unit_tests_dbms`; CA test sources live under `src/Disks/tests/gtest_cas_*.cpp`. The only tolerated baseline-red test is `CaWiringOps.FreezeViaHardLinksIntoShadow`.

**TLA+ run mechanics** (exact; from `docs/superpowers/models/`)
- Run one config (the Phase-0 wrapper hardcodes the module):
  ```bash
  cd docs/superpowers/models
  ./run_gc_partmanifest.sh <Cfg-basename-without-.cfg>
  ```
  which expands to
  ```bash
  java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
       -metadir ../../../tmp/tlc-meta -workers auto -config <Cfg>.cfg CaGcRootLocalPartManifestCore.tla
  ```
  (For a long run set `TLC_JAVA_OPTS=-Xmx48g`.)
- **PASS (HOLD config)** = exit 0 and the log contains `Model checking completed. No error has been found.`
- A **`_sab_*` / `_buggy` config is CORRECT only when it FAILS** with `Error: Invariant <NAME> is violated.` (or `Temporal properties were violated.`). A zero exit on a `_sab_*` config is a **suite failure** (`UNEXPECTED PASS`).

---

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

- The model proves the *skip rule* only: skipping an unchanged shard (listed token == folded token) preserves `INV_NO_DANGLE`/`INV_NO_LOSS`; skipping a changed shard (token advanced) breaks `INV_NO_DANGLE`. On-wire token encoding (how a backend surfaces a per-key token through LIST) is a code concern, not modeled.
- Token capability (`supportsListTokens`) is modeled abstractly by a `TokenObservable` flag: when FALSE, every shard is force-read (no skip is ever taken), which is trivially safe; when TRUE, the skip rule applies. The code Task 2 ships the real probe; the model only needs to prove the rule under both observability modes.
- LIST never shrinks the registry universe: the discover universe is `discoverUniverse()` (registry authority) unioned with any LIST-only additions, never a LIST-only intersection. This is the existing `discoverUniverse` contract (registry is authority); Phase 2 must not regress it.

## Canonical Contract Consumed (from Phase 1d) {#canonical-contract-consumed-from-phase-1d}

These names are produced by Phase 1d and consumed verbatim here. **Do not redefine them; reference them.**

- `GenerationSeal{generation, parent_generation, per_ns_shard(ShardCoverage{classification, folded_token, folded_cursor}), ...}` — the write-once generation completeness record. Phase 2 persists, per root shard, the folded token + folded cursor inside `ShardCoverage`, and reads them back in `discover`.
- `ShardCoverage{classification, folded_token, folded_cursor}` — the per-`(RootNamespace, shard)` coverage entry inside the seal. `classification` records how the shard was handled this round (read / skipped / minted); `folded_token` is the root-shard object token folded through; `folded_cursor` is the journal cursor folded through.
- `CasGc` round = `discover → fold → retire → fence → recheck → delete → trim` (the part-manifest model's round; no cascade step). Phase 2 changes only `discover`.
- `CasLayout::generationSealKey(gen)` — key of the `GenerationSeal` object for a generation.
- `CasBlobInDegree` — the streaming blob in-degree generation built from owner-transition manifest streams (Phase 1d). Phase 2 does not change it.

## File Structure {#file-structure}

All C++ paths under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (abbreviated `CA/`); gtests under `src/Disks/tests/`.

- **Modify:** `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` — add the `EnableTokenDiff` stage actions + invariant wiring (Task 1).
- **Create:** `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5_tokendiff.cfg` — positive skip-safety stage (must HOLD).
- **Create:** `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_skipchangedshard.cfg` — negative control (must VIOLATE `INV_NO_DANGLE`).
- **Modify:** `CA/Core/CasBackend.h` — add `bool supportsListTokens()` to the `Backend` seam (Task 2).
- **Modify:** the concrete backends that implement `Backend` (`CA/Core/CasInMemoryBackend.*`, `CA/Core/CasObjectStorageBackend.*`, `CA/Core/CasInstrumentedBackend.*`) — implement `supportsListTokens` (Task 2).
- **Modify:** `CA/Core/CasGcFormats.*` (or the file Phase 1d placed `ShardCoverage` in — confirm from the Phase 1d ground-truth report; likely `CA/Core/CasGenerationSeal.*`) — add `folded_token`/`folded_cursor` round-trip to `ShardCoverage` if Phase 1d left either unwired (Task 3).
- **Modify:** `CA/Core/CasGc.*` — `discover` token-diff skip rule (Task 4); fail-closed fallback (Task 5).
- **Create:** `src/Disks/tests/gtest_cas_gc_token_diff.cpp` — gtests `CasGcDiscovery` + `CasBackendListTokens` + `CasShardCoverageRoundTrip` (Tasks 2–5).

## Modeled Vocabulary Added This Phase {#modeled-vocabulary-added-this-phase}

CONSTANTS added to `CaGcRootLocalPartManifestCore.tla`: `EnableTokenDiff` (positive flag, gates the skip stage), `TokenObservable` (abstract `supportsListTokens`), `SabotageSkipChangedShard` (negative control).

VARIABLES added: `foldedTok` (`[Namespaces -> 0..MaxToken]` the persisted folded root-shard token per namespace, the `ShardCoverage.folded_token` abstraction) and `rootTok` (`[Namespaces -> 0..MaxToken]` the live root-shard object token, advanced by any owner transition).

Actions added: `GDiscoverSkip(n)` (skip a shard whose listed token equals the folded token) and `GDiscoverRead(n)` (read a shard's body and refresh the folded token). `SabotageSkipChangedShard` lets `GDiscoverSkip` fire when `rootTok[n] # foldedTok[n]`.

---

## Tasks {#tasks}

### Task 1: Model extension — prove skip-of-unchanged is safe, skip-of-changed dangles {#task-1-model-extension-prove-skip-of-unchanged-is-safe-skip-of-changed-dangles}

**This task is the R0 gate. Tasks 2–6 may not start until this task's two configs are correct.**

**Files:**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5_tokendiff.cfg`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_skipchangedshard.cfg`

**Interfaces:**
- Consumes (from Phase 0): the module's existing `VARIABLES`, `vars` tuple, `Init`, `Next`, `INV_NO_DANGLE`, `INV_NO_LOSS`, `StateConstraint`, and the `cursor`/`fencePos`/`journal` GC pipeline (the round skeleton `discover → fold → retire → fence → recheck → delete → trim`).
- Produces: `EnableTokenDiff`, `TokenObservable`, `SabotageSkipChangedShard` CONSTANTS; `foldedTok`, `rootTok` VARIABLES; `GDiscoverSkip`, `GDiscoverRead` actions; the proof that the skip rule preserves `INV_NO_DANGLE`/`INV_NO_LOSS`.

- [ ] **Step 1: Add the three CONSTANTS.** In `CaGcRootLocalPartManifestCore.tla`, extend the `CONSTANTS` block (append to the existing `Enable*` line group and the `Sabotage*` group):

```tla
CONSTANTS
    \* ... existing constants (Namespaces, Writers, ..., EnablePrecommit, ...) ...
    EnableTokenDiff,     \* TRUE -> discover MAY skip an unchanged shard's body read
    TokenObservable,     \* TRUE -> LIST surfaces a per-shard token (supportsListTokens); FALSE -> always read
    \* ... existing Sabotage* constants ...
    SabotageSkipChangedShard   \* skip a shard whose root token actually advanced (must dangle)
```

- [ ] **Step 2: Add the two VARIABLES.** Append `foldedTok, rootTok` to the `VARIABLES` declaration and to the `vars` tuple. The `vars` tuple must list every variable or TLC rejects the spec, so add them at the end:

```tla
VARIABLES
    \* ... existing variables (present, tokOf, ..., mfCleanup, sweepEligible) ...
    foldedTok,   \* [Namespaces -> Toks \cup {0}] persisted ShardCoverage.folded_token (last folded root token)
    rootTok      \* [Namespaces -> Toks \cup {0}] live root-shard object token; any owner transition advances it

vars == << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
           journal, blobIndeg, blobEdges, everEdged, seal, gcRound, gcPhase, roundOf, fencePos,
           cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, sweepEligible,
           foldedTok, rootTok >>
```

- [ ] **Step 3: Initialise the two VARIABLES.** Append to `Init` (a fresh pool has no folded token and a zero live token):

```tla
    /\ foldedTok = [n \in Namespaces |-> 0]
    /\ rootTok = [n \in Namespaces |-> 0]
```

- [ ] **Step 4: Advance `rootTok` on every owner transition.** Any action that appends an `OwnerTransition` to `journal[n]` (the Phase-0 `WPrecommitAdd`, `WPromote`, `WPublishCommitted`, `WDropRef`, `WRepoint`, `WAbandonPrecommit` actions) must also bump `rootTok[n]` so a body read can observe the new token. Add this conjunct to each such action's body, capped at `MaxToken` so `TypeOK` holds:

```tla
    /\ rootTok' = [rootTok EXCEPT ![n] = IF rootTok[n] < MaxToken THEN rootTok[n] + 1 ELSE rootTok[n]]
```

Every other variable each such action previously left `UNCHANGED` must now also leave `foldedTok` unchanged: add `foldedTok` to each action's `UNCHANGED << ... >>` list (and remove `rootTok` from it, since the action now changes `rootTok`).

- [ ] **Step 5: Add the discovery actions.** In the helpers/actions section (near the GC pipeline actions), add:

```tla
\* The token-diff skip. A shard is skippable iff LIST surfaces a token (TokenObservable) AND the
\* listed token equals the persisted folded token. A SAFE skip folds nothing and advances nothing:
\* the durable folded state already covers every transition up to foldedTok[n]. The negative control
\* SabotageSkipChangedShard drops the equality guard, skipping a shard whose root token advanced.
GDiscoverSkip(n) ==
    /\ EnableTokenDiff
    /\ TokenObservable
    /\ (rootTok[n] = foldedTok[n] \/ SabotageSkipChangedShard)
    /\ UNCHANGED vars        \* a skip reads nothing, folds nothing, advances no cursor or folded token

\* The body read. Always legal; refreshes the persisted folded token to the live token so a later
\* round may skip. The fold of the journal records it covers is the existing GFoldTransition; this
\* action models ONLY the discovery decision and the folded-token refresh, leaving the fold pipeline
\* (cursor/edges) to GFoldTransition.
GDiscoverRead(n) ==
    /\ foldedTok' = [foldedTok EXCEPT ![n] = rootTok[n]]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, seal, gcRound, gcPhase, roundOf, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, sweepEligible,
                    rootTok >>
```

- [ ] **Step 6: Wire the actions into `Next`.** Add the two discovery disjuncts to the existing `Next == ... \/ ...` (do not remove existing disjuncts):

```tla
    \/ \E n \in Namespaces : GDiscoverSkip(n)
    \/ \E n \in Namespaces : GDiscoverRead(n)
```

- [ ] **Step 7: Add `TypeOK` clauses for the new variables.** Extend `TypeOK`:

```tla
    /\ foldedTok \in [Namespaces -> 0..MaxToken]
    /\ rootTok \in [Namespaces -> 0..MaxToken]
```

- [ ] **Step 8: Write `stage5_tokendiff.cfg`** (must HOLD). Copy `CaGcRootLocalPartManifestCore_stage4.cfg`, then set `EnableTokenDiff = TRUE`, `TokenObservable = TRUE`, keep `SabotageSkipChangedShard = FALSE` (and every other `Sabotage* = FALSE`). Keep the same invariant lines as `stage4` plus the no-dangle/no-loss core. The cfg body (constants block elided to the deltas — copy the rest from `stage4.cfg` verbatim):

```
SPECIFICATION Spec
CONSTANTS
    \* ... copy every constant assignment from CaGcRootLocalPartManifestCore_stage4.cfg ...
    EnableTokenDiff = TRUE
    TokenObservable = TRUE
    SabotageSkipChangedShard = FALSE
CONSTRAINT StateConstraint
INVARIANT TypeOK
INVARIANT INV_JOURNAL_COVERAGE
INVARIANT INV_NO_DANGLE
INVARIANT INV_NO_LOSS
INVARIANT INV_NO_RETURN
INVARIANT BlobInDegreeMatchesActiveManifests
```

(If `stage4.cfg` does not yet assign `EnableTokenDiff`/`TokenObservable`/`SabotageSkipChangedShard`, every existing `_stage*`/`_sab_*`/`_live`/`_witness` cfg must also gain the three new constant assignments — TLC errors on an unassigned CONSTANT. Set `EnableTokenDiff = FALSE`, `TokenObservable = FALSE`, `SabotageSkipChangedShard = FALSE` in each pre-existing cfg so their behavior is unchanged. This is a mechanical edit across the cfg suite; do it in this step.)

- [ ] **Step 9: Run `stage5_tokendiff` — must HOLD.**

Run: `cd docs/superpowers/models && TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage5_tokendiff`
Expected: log line `Model checking completed. No error has been found.` and `exit=0`. If TLC finds a trace here (all sabotage off), the model is wrong, not the cfg — the skip guard must make a skip fold-neutral; fix `GDiscoverSkip` until it holds.

- [ ] **Step 10: Write `sab_skipchangedshard.cfg`** (must VIOLATE `INV_NO_DANGLE`). Copy `stage5_tokendiff.cfg`, flip `SabotageSkipChangedShard = TRUE`, and narrow the invariant list to the single targeted invariant for a fast counterexample (the `CaIncarnationCore` convention):

```
SPECIFICATION Spec
CONSTANTS
    \* ... same constants as stage5_tokendiff.cfg ...
    EnableTokenDiff = TRUE
    TokenObservable = TRUE
    SabotageSkipChangedShard = TRUE
CONSTRAINT StateConstraint
INVARIANT INV_NO_DANGLE
```

- [ ] **Step 11: Run `sab_skipchangedshard` — must FAIL with `INV_NO_DANGLE`.**

Run: `cd docs/superpowers/models && TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_skipchangedshard`
Expected: `Error: Invariant INV_NO_DANGLE is violated.` and a nonzero `exit=`. A zero exit is a gate failure (`UNEXPECTED PASS`): the sabotage did not reach a dangle, meaning the model cannot express a publish that the skip then misses — fix `GDiscoverSkip`/`rootTok` until the changed-shard skip drops a still-referenced manifest.

- [ ] **Step 12: Re-run the whole suite GREEN** (regression — Step 8's mechanical cfg edit must not have broken any pre-existing config). Use a subagent to analyze the combined log if it is long.

Run:
```bash
cd docs/superpowers/models
for s in stage0 stage1 stage2 stage3 stage4 stage5_tokendiff live ; do
  TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_$s ; done
for c in reusemanifestid twoowners splitpromote missingbodyactivated commitskipblobreval precommitlessprotect \
         noorphansweep wholesaleprefixdelete frozenseqauthority missingcommittedempty deletebodybeforedecrements \
         cutoverclaim roundvisibilityearly nofence trimunincorporated unconddelete reusedtag barenonce keybyrefnotid \
         acceptnamespacemismatch acceptrefmismatch mutableasreachability promoteaftermissingbody skipchangedshard ; do
  TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_$c && echo "UNEXPECTED PASS: $c"
done
```
Expected: every `stage*`/`live` row prints `No error`; every `_sab_*` row prints its expected invariant violation with a nonzero exit; **no** `UNEXPECTED PASS` line appears.

- [ ] **Step 13: Commit.**

```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5_tokendiff.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_skipchangedshard.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage0.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage1.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage2.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage3.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage4.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_live.cfg
git commit -m "CA GC phase2: model ext — token-diff skip stage5 + sab_skipchangedshard (gate green)"
```

(If the witness cfgs from Phase 0 Task 9 also gained the three constants in Step 8, `git add` them too.)

---

### Task 2: LIST-token capability probe on the backend seam {#task-2-list-token-capability-probe-on-the-backend-seam}

**Files:**
- Modify: `CA/Core/CasBackend.h`
- Modify: `CA/Core/CasInMemoryBackend.h` / `CA/Core/CasInMemoryBackend.cpp`
- Modify: `CA/Core/CasObjectStorageBackend.h` / `CA/Core/CasObjectStorageBackend.cpp`
- Modify: `CA/Core/CasInstrumentedBackend.h` / `CA/Core/CasInstrumentedBackend.cpp`
- Create: `src/Disks/tests/gtest_cas_gc_token_diff.cpp`

**Interfaces:**
- Consumes: the `Backend` abstract class in `CasBackend.h` (the storage seam with `get`/`head`/`putIfAbsent`/`putIfAbsentStream`/`putOverwrite`/`casPut`/`deleteExact`/`list`).
- Produces: `virtual bool supportsListTokens() const = 0;` on `Backend` — TRUE iff this backend can surface a per-key incarnation token through `list` (i.e. the token-diff skip is permissible against it). `CasInstrumentedBackend` forwards to its wrapped backend.

- [ ] **Step 1: Write the failing test.** Create `src/Disks/tests/gtest_cas_gc_token_diff.cpp`:

```cpp
#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>

using namespace DB::Cas;

/// The in-memory backend mints a strictly-monotonic per-incarnation token (see CasBackend.h token
/// contract), so it CAN surface a per-key token through list: supportsListTokens must be TRUE.
TEST(CasBackendListTokens, InMemorySupportsListTokens)
{
    InMemoryBackend backend;
    EXPECT_TRUE(backend.supportsListTokens());
}

/// A trivial backend that does not surface list tokens must be able to report FALSE — the seam
/// must be overridable both ways so discover can fall closed to body reads.
namespace
{
struct NoListTokenBackend final : Backend
{
    std::optional<GetResult> get(const String &, Range) override { return std::nullopt; }
    HeadResult head(const String &) override { return HeadResult{}; }
    PutResult putIfAbsent(const String &, const String &, const ObjectMeta &) override { return {PutOutcome::Done, {}}; }
    WriteSinkPtr putIfAbsentStream(const String &, const ObjectMeta &) override { return nullptr; }
    PutResult putOverwrite(const String &, const String &, const Token &, const ObjectMeta &) override { return {PutOutcome::PreconditionFailed, {}}; }
    CasResult casPut(const String &, const String &, const std::optional<Token> &, const ObjectMeta &) override { return {CasOutcome::Conflict, {}}; }
    DeleteOutcome deleteExact(const String &, const Token &) override { return {}; }
    ListPage list(const String &, const String &, size_t) override { return {}; }
    bool supportsListTokens() const override { return false; }
};
}

TEST(CasBackendListTokens, OverridableToFalse)
{
    NoListTokenBackend backend;
    EXPECT_FALSE(backend.supportsListTokens());
}
```

- [ ] **Step 2: Run the test to verify it fails to compile.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t2.log 2>&1` (no `-j`). Analyze `build/build_cas_phase2_t2.log` with a subagent.
Expected: compile error — `supportsListTokens` is not a member of `Backend` (and `InMemoryBackend` has no such method).

- [ ] **Step 3: Add the pure virtual to the `Backend` seam.** In `CasBackend.h`, inside `class Backend`, after `list` (around the `deleteExact`/`list` declarations):

```cpp
    /// TRUE iff this backend can surface a per-key incarnation token through `list`, so GC discover
    /// may use the token-diff skip (a listed root token equal to the persisted folded token ⇒ skip
    /// the body read). FALSE ⇒ discover MUST read every root-shard body (fail closed to body reads).
    /// This is a capability fact about the LIST seam, not a per-call result; it never changes for a
    /// live backend. S3 ETag is content-derived and list-surfaceable; the in-memory backend mints a
    /// monotonic token it can also surface; a backend that cannot must return FALSE.
    virtual bool supportsListTokens() const = 0;
```

- [ ] **Step 4: Implement it on the in-memory backend.** In `CasInMemoryBackend.h` (the in-memory backend surfaces its monotonic token through `list`):

```cpp
    bool supportsListTokens() const override { return true; }
```

- [ ] **Step 5: Implement it on the object-storage backend.** In `CasObjectStorageBackend.h`/`.cpp` (S3 ETag is content-derived and list-surfaceable):

```cpp
    bool supportsListTokens() const override { return true; }
```

- [ ] **Step 6: Implement it on the instrumented backend (forward to the wrapped backend).** In `CasInstrumentedBackend.h`/`.cpp`, forward to the wrapped backend so instrumentation is transparent to the capability:

```cpp
    bool supportsListTokens() const override { return inner->supportsListTokens(); }
```

(Use the instrumented backend's actual wrapped-backend member name — confirm it from `CasInstrumentedBackend.h`; it is the same member every other forwarding method uses.)

- [ ] **Step 7: Run the test to verify it passes.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t2b.log 2>&1` then `build/src/unit_tests_dbms --gtest_filter='CasBackendListTokens*' > test_cas_phase2_t2.log 2>&1`. Analyze both logs with a subagent.
Expected: both `CasBackendListTokens.InMemorySupportsListTokens` and `CasBackendListTokens.OverridableToFalse` PASS.

- [ ] **Step 8: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInstrumentedBackend.h \
        src/Disks/tests/gtest_cas_gc_token_diff.cpp
git commit -m "CA GC phase2: add supportsListTokens capability to the Backend seam"
```

---

### Task 3: Persist folded root-shard token + cursor in `ShardCoverage` (round-trip) {#task-3-persist-folded-root-shard-token-cursor-in-shardcoverage-round-trip}

**Files:**
- Modify: `CA/Core/CasGenerationSeal.*` (the file Phase 1d defined `GenerationSeal`/`ShardCoverage` in — confirm exact path from the Phase 1d ground-truth report; the overview's file map places it at `CA/Core/CasGenerationSeal.h`/`.cpp`).
- Modify: `src/Disks/tests/gtest_cas_gc_token_diff.cpp`

**Interfaces:**
- Consumes: `ShardCoverage{classification, folded_token, folded_cursor}` and `GenerationSeal{generation, parent_generation, per_ns_shard, ...}` from Phase 1d, plus its `encodeGenerationSeal`/`decodeGenerationSeal` codec (confirm exact codec names from the Phase 1d report).
- Produces: a verified round-trip — a `GenerationSeal` carrying a `ShardCoverage` with a non-zero `folded_token` (a `Token`) and `folded_cursor` (a `uint64_t`) encodes and decodes byte-stably.

> **Note:** Phase 1d's contract already lists `folded_token` and `folded_cursor` as `ShardCoverage` fields. If the Phase 1d codec already encodes and decodes both, this task is a *characterization test only* (Steps 1, 7, 8 — assert the round-trip, no codec change). Only if the Phase 1d report shows either field is declared but not yet serialized do Steps 3–6 apply. Confirm from the report before editing the codec.

- [ ] **Step 1: Write the failing test.** Append to `src/Disks/tests/gtest_cas_gc_token_diff.cpp` (add the `CasGenerationSeal.h` include at the top of the file):

```cpp
// add near the top includes:
// #include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>

/// The token-diff skip reads folded_token + folded_cursor back out of the persisted ShardCoverage,
/// so both must survive an encode/decode round-trip exactly.
TEST(CasShardCoverageRoundTrip, FoldedTokenAndCursorSurviveEncodeDecode)
{
    GenerationSeal seal;
    seal.generation = 12;
    seal.parent_generation = 11;

    ShardCoverage cov;
    cov.classification = ShardCoverage::Classification::Read;   /// confirm enum name from Phase 1d
    cov.folded_token = Token{"etag-abc"};                        /// confirm Token construction from CasToken.h
    cov.folded_cursor = 7;

    seal.per_ns_shard[{RootNamespace{"srv1/tbl"}, /*shard*/ 0}] = cov;   /// confirm key type from Phase 1d

    const String bytes = encodeGenerationSeal(seal);
    const GenerationSeal decoded = decodeGenerationSeal(bytes);

    const ShardCoverage & got = decoded.per_ns_shard.at({RootNamespace{"srv1/tbl"}, 0});
    EXPECT_EQ(got.folded_token, Token{"etag-abc"});
    EXPECT_EQ(got.folded_cursor, 7u);
    EXPECT_EQ(got.classification, ShardCoverage::Classification::Read);
}
```

(The exact `ShardCoverage` field/enum names, the `per_ns_shard` map key type, and the codec function names come from Phase 1d — adjust this test to match the Phase 1d ground-truth report, keeping the assertion intent: `folded_token` and `folded_cursor` round-trip.)

- [ ] **Step 2: Run the test to verify it fails.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t3.log 2>&1` then `build/src/unit_tests_dbms --gtest_filter='CasShardCoverageRoundTrip*' > test_cas_phase2_t3.log 2>&1`. Analyze both logs with a subagent.
Expected: either a compile error (if a field is missing) or a FAIL on the `folded_token`/`folded_cursor` assertion (if the codec drops the field). If it PASSES immediately, Phase 1d already serializes both — this is then a passing characterization test; record that and skip to Step 7.

- [ ] **Step 3: Add the fields to `ShardCoverage` if absent.** In `CasGenerationSeal.h`, ensure `ShardCoverage` declares both (only if the Phase 1d report shows they are missing):

```cpp
struct ShardCoverage
{
    enum class Classification : uint8_t { Read, Skipped, Minted };   /// match Phase 1d's existing enum
    Classification classification = Classification::Read;
    Token folded_token;        /// the root-shard object token folded through this round (Phase 2 skip key)
    uint64_t folded_cursor = 0;   /// the journal cursor folded through this round
};
```

- [ ] **Step 4: Encode both fields.** In `CasGenerationSeal.cpp`, inside the per-`ShardCoverage` branch of `encodeGenerationSeal`, write `folded_token` and `folded_cursor` (match the file's existing encoding style — the overview specifies generation seals are control-plane records, so follow Phase 1d's chosen envelope, e.g. its protobuf or strict-binary writer):

```cpp
    // within the loop over per_ns_shard, after writing `classification`:
    encodeToken(out, cov.folded_token);        /// reuse the Token encoder this codec already uses
    writeBinaryLittleEndian(cov.folded_cursor, out);
```

- [ ] **Step 5: Decode both fields.** In `CasGenerationSeal.cpp`, the matching `decodeGenerationSeal` branch:

```cpp
    // within the per-shard decode, after reading `classification`:
    cov.folded_token = decodeToken(in);
    readBinaryLittleEndian(cov.folded_cursor, in);
```

- [ ] **Step 6: Run the test to verify it passes.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t3b.log 2>&1` then `build/src/unit_tests_dbms --gtest_filter='CasShardCoverageRoundTrip*' > test_cas_phase2_t3c.log 2>&1`. Analyze both logs with a subagent.
Expected: `CasShardCoverageRoundTrip.FoldedTokenAndCursorSurviveEncodeDecode` PASS.

- [ ] **Step 7: Guard against regression — run the existing seal/format tests.**

Run: `build/src/unit_tests_dbms --gtest_filter='*GenerationSeal*:CasGcFormats*' > test_cas_phase2_t3d.log 2>&1`. Analyze with a subagent.
Expected: all PASS (the field addition must not break Phase 1d's own seal tests).

- [ ] **Step 8: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp \
        src/Disks/tests/gtest_cas_gc_token_diff.cpp
git commit -m "CA GC phase2: round-trip folded root-shard token + cursor in ShardCoverage"
```

---

### Task 4: `discover` skips a root shard iff listed token == persisted folded token {#task-4-discover-skips-a-root-shard-iff-listed-token-persisted-folded-token}

**Files:**
- Modify: `CA/Core/CasGc.h` (`discoverUniverse`/`shardsToVisit` neighborhood, `:272`/`:280`)
- Modify: `CA/Core/CasGc.cpp` (the `discover` step body)
- Modify: `src/Disks/tests/gtest_cas_gc_token_diff.cpp`

**Interfaces:**
- Consumes: `discoverUniverse()` (registry authority for the namespace universe, `CasGc.h:272`); `shardsToVisit(ns)` (`CasGc.h:280`); `Backend::supportsListTokens()` (Task 2); `ShardCoverage.folded_token`/`folded_cursor` from the previously sealed `GenerationSeal` (Task 3, read via `CasLayout::generationSealKey`); `Backend::list` (`ListPage`) for the listed root-shard token; the round skeleton `discover → fold → ...`.
- Produces: a `discover` that returns, per `(RootNamespace, shard)`, a decision `{Skip, Read}` such that **Skip** is taken iff `supportsListTokens()` AND a listed token for that shard's `rootShardKey` is present AND equals the sealed `ShardCoverage.folded_token`; otherwise **Read**. The discover universe is `discoverUniverse()` unioned with any LIST-only namespaces, never intersected with LIST.

- [ ] **Step 1: Write the failing test (skip when equal).** Append to `gtest_cas_gc_token_diff.cpp`. Use the existing GC round test scaffold (`cas_test_helpers.h`, an `InMemoryBackend`-backed `Store`, a `Gc` — model it on `gtest_cas_gc_round.cpp`):

```cpp
// includes to add at top (match gtest_cas_gc_round.cpp):
// #include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
// #include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
// #include <Disks/tests/cas_test_helpers.h>

/// After a round folds a shard and seals its folded_token, a second round whose root-shard token is
/// UNCHANGED must SKIP the body read for that shard (decision == Skip) — the token-diff accelerator.
TEST(CasGcDiscovery, SkipsUnchangedShardWhenListedTokenEqualsFoldedToken)
{
    auto store = DB::Cas::tests::makeInMemoryStore();   /// confirm helper name from cas_test_helpers.h
    // ... publish one committed ref into namespace N shard S, run one full round so the seal records
    //     ShardCoverage.folded_token == the live root-shard token ...
    Gc gc(store, DB::Cas::tests::u128Of(1));
    gc.runRegularRound();   /// round 1: reads + folds + seals folded_token for (N, S)

    // round 2 with NO new publish: the discover decision for (N, S) must be Skip.
    const auto decisions = gc.discoverDecisionsForTest();   /// test seam added in Step 3
    ASSERT_FALSE(decisions.empty());
    for (const auto & [ns_shard, decision] : decisions)
        EXPECT_EQ(decision, Gc::DiscoverDecision::Skip) << "shard " << ns_shard.first.string() << "/" << ns_shard.second;
}
```

- [ ] **Step 2: Write the failing test (body-read on mismatch and on missing token).** Append:

```cpp
/// A shard whose root-shard token ADVANCED since the sealed folded_token (a new publish) must be
/// READ, not skipped. And a backend that cannot surface list tokens must read EVERY shard.
TEST(CasGcDiscovery, ReadsShardWhenTokenAdvancedOrMissing)
{
    auto store = DB::Cas::tests::makeInMemoryStore();
    // ... publish into (N, S); run round 1 (seals folded_token) ...
    Gc gc(store, DB::Cas::tests::u128Of(1));
    gc.runRegularRound();

    // a NEW publish into (N, S) advances its root-shard token past the sealed folded_token:
    // ... publish a second ref / drop+repoint into (N, S) ...

    const auto decisions = gc.discoverDecisionsForTest();
    bool saw_read = false;
    for (const auto & [ns_shard, decision] : decisions)
        if (decision == Gc::DiscoverDecision::Read)
            saw_read = true;
    EXPECT_TRUE(saw_read) << "an advanced shard must be read, never skipped";
}

/// LIST never shrinks the registry universe: every namespace the registry knows is present in the
/// discover decisions even if LIST returns fewer keys.
TEST(CasGcDiscovery, RegistryUniverseNeverShrunkByList)
{
    auto store = DB::Cas::tests::makeInMemoryStore();
    // ... register two namespaces N1, N2 in the registry; publish into N1 only ...
    Gc gc(store, DB::Cas::tests::u128Of(1));
    const auto decisions = gc.discoverDecisionsForTest();

    std::set<DB::Cas::RootNamespace> seen;
    for (const auto & [ns_shard, decision] : decisions)
        seen.insert(ns_shard.first);
    EXPECT_EQ(seen.count(DB::Cas::RootNamespace{"N1"}), 1u);   /// confirm helper namespace names
    EXPECT_EQ(seen.count(DB::Cas::RootNamespace{"N2"}), 1u);   /// registry authority, not LIST
}
```

- [ ] **Step 3: Run the tests to verify they fail.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t4.log 2>&1`. Analyze with a subagent.
Expected: compile error — `Gc::DiscoverDecision` and `Gc::discoverDecisionsForTest` do not exist yet.

- [ ] **Step 4: Add the decision enum and the discover seam.** In `CasGc.h`, in the `public:` section near the other report/preview structs:

```cpp
    /// What discover decided for one root shard. Skip = the listed root token equals the persisted
    /// ShardCoverage.folded_token, so the body read is elided; Read = read the root-shard body and
    /// re-fold (token missing/ambiguous/stale, or supportsListTokens() is false — fail closed to a
    /// body read). LIST never removes a registry namespace from the universe.
    enum class DiscoverDecision : uint8_t { Skip, Read };

    /// TEST SEAM (write-free): the per-(namespace, shard) discover decisions the next round would
    /// make, derived from the DURABLE sealed ShardCoverage.folded_token + a single LIST. No CAS, no
    /// delete, no fold. Mirrors `previewDeletes`'s write-free-diagnostic contract.
    std::map<std::pair<RootNamespace, uint64_t>, DiscoverDecision> discoverDecisionsForTest();
```

- [ ] **Step 5: Implement the token-diff decision in `discover`.** In `CasGc.cpp`, factor the decision into a private helper the round's `discover` and `discoverDecisionsForTest` both call. The rule (spec §Discovery, verbatim): listed token == folded token ⇒ Skip; token missing/ambiguous/stale/unsupported ⇒ Read; LIST never shrinks the registry universe; fail closed to body reads.

```cpp
    // private helper in CasGc.cpp:
    std::map<std::pair<RootNamespace, uint64_t>, Gc::DiscoverDecision>
    Gc::computeDiscoverDecisions(const GenerationSeal & sealed)
    {
        std::map<std::pair<RootNamespace, uint64_t>, DiscoverDecision> out;

        // 1. The universe is the REGISTRY universe — authority. LIST is only an accelerator and may
        //    NEVER remove a namespace from it.
        const auto universe = discoverUniverse();

        // 2. If the backend cannot surface list tokens, every shard is read (fail closed).
        const bool token_observable = store->backend().supportsListTokens();

        // 3. One LIST over the roots prefix to read per-shard tokens (when observable).
        std::map<String, Token> listed_token;   /// rootShardKey -> token, from a single LIST sweep
        if (token_observable)
            listed_token = listRootShardTokens();   /// LIST the roots prefix, keep key->token

        for (const auto & [ns, shard] : universe)
        {
            DiscoverDecision decision = DiscoverDecision::Read;   /// default: read (fail closed)
            if (token_observable)
            {
                const String key = store->layout().rootShardKey(ns, shard);
                const auto it = listed_token.find(key);
                const ShardCoverage * cov = sealed.find(ns, shard);   /// nullptr if not yet sealed
                // SKIP iff: LIST surfaced a token AND a prior generation sealed a folded_token AND
                //           the two tokens are equal. Any of: token missing, no prior coverage, or
                //           tokens differ ⇒ Read (fail closed).
                if (it != listed_token.end() && cov != nullptr && tokensEqual(it->second, cov->folded_token))
                    decision = DiscoverDecision::Skip;
            }
            out[{ns, shard}] = decision;
        }
        return out;
    }
```

Then wire the real `discover`/`fold` step so a **Skip** decision elides the body read (it folds nothing — the durable folded state already covers it) and a **Read** decision reads the body and re-folds (the existing fold path), refreshing the `ShardCoverage.folded_token`/`folded_cursor` the round seals (Task 3). Implement `discoverDecisionsForTest` as a thin write-free wrapper that loads the sealed `GenerationSeal` (via `CasLayout::generationSealKey(state.snap_generation)` or the Phase-1d seal pointer) and returns `computeDiscoverDecisions(sealed)`. Add the small helpers `listRootShardTokens` (a `Backend::list` sweep keeping `key -> token`), `GenerationSeal::find(ns, shard)`, and `tokensEqual` (token equality already exists in `CasToken.h` — reuse it; do not reinvent).

- [ ] **Step 6: Run the tests to verify they pass.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t4b.log 2>&1` then `build/src/unit_tests_dbms --gtest_filter='CasGcDiscovery*' > test_cas_phase2_t4.log 2>&1`. Analyze both logs with a subagent.
Expected: `CasGcDiscovery.SkipsUnchangedShardWhenListedTokenEqualsFoldedToken`, `CasGcDiscovery.ReadsShardWhenTokenAdvancedOrMissing`, and `CasGcDiscovery.RegistryUniverseNeverShrunkByList` all PASS.

- [ ] **Step 7: Guard against regression — run the full GC round suite.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGc*:CasGcRound*' > test_cas_phase2_t4c.log 2>&1`. Analyze with a subagent.
Expected: all PASS (the skip must be behavior-preserving — a skipped shard's blob in-degree and delete decisions are identical to having read it).

- [ ] **Step 8: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_token_diff.cpp
git commit -m "CA GC phase2: discover skips an unchanged root shard on token-diff (registry universe preserved)"
```

---

### Task 5: Fail-closed fallback to body reads on any ambiguity {#task-5-fail-closed-fallback-to-body-reads-on-any-ambiguity}

**Files:**
- Modify: `CA/Core/CasGc.cpp` (the `computeDiscoverDecisions` helper from Task 4)
- Modify: `src/Disks/tests/gtest_cas_gc_token_diff.cpp`

**Interfaces:**
- Consumes: `computeDiscoverDecisions` (Task 4); `DiscoverDecision` (Task 4).
- Produces: the hardened fail-closed rule — a shard is **Read** (never Skip) whenever the listed token is absent, the same `rootShardKey` appears more than once in the LIST sweep (ambiguous), there is no prior `ShardCoverage` for it, or `supportsListTokens()` is false. Skip is taken only on an unambiguous, present, equal token.

- [ ] **Step 1: Write the failing tests.** Append to `gtest_cas_gc_token_diff.cpp`:

```cpp
/// A backend that cannot surface list tokens (supportsListTokens()==false) must Read every shard,
/// even an unchanged one — there is no token to compare, so skipping would be unsafe.
TEST(CasGcDiscovery, FailsClosedToReadWhenTokensUnobservable)
{
    // A store wrapping a backend whose supportsListTokens() is false. Build a NoListTokenBackend-backed
    // store (or wrap InMemoryBackend with a capability-suppressing decorator) that still answers the
    // registry + a single sealed shard.
    auto store = DB::Cas::tests::makeStoreWithListTokensUnobservable();   /// confirm/add helper
    Gc gc(store, DB::Cas::tests::u128Of(1));
    gc.runRegularRound();   /// seals folded_token

    const auto decisions = gc.discoverDecisionsForTest();
    ASSERT_FALSE(decisions.empty());
    for (const auto & [ns_shard, decision] : decisions)
        EXPECT_EQ(decision, Gc::DiscoverDecision::Read) << "unobservable tokens must force a body read";
}

/// A shard the seal has NO prior coverage for (never folded) must be Read, never Skipped — there is
/// no folded_token to compare against, so fail closed.
TEST(CasGcDiscovery, FailsClosedToReadWhenNoPriorCoverage)
{
    auto store = DB::Cas::tests::makeInMemoryStore();
    // ... register namespace N shard S in the registry, publish into it, but do NOT run a round
    //     (so no GenerationSeal has folded_token for (N, S) yet) ...
    Gc gc(store, DB::Cas::tests::u128Of(1));
    const auto decisions = gc.discoverDecisionsForTest();
    ASSERT_FALSE(decisions.empty());
    for (const auto & [ns_shard, decision] : decisions)
        EXPECT_EQ(decision, Gc::DiscoverDecision::Read) << "no prior ShardCoverage ⇒ must read";
}
```

- [ ] **Step 2: Run the tests to verify they fail or are under-covered.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t5.log 2>&1` then `build/src/unit_tests_dbms --gtest_filter='CasGcDiscovery.FailsClosed*' > test_cas_phase2_t5.log 2>&1`. Analyze both logs with a subagent.
Expected: a compile error if the helper `makeStoreWithListTokensUnobservable` is missing (add it to `cas_test_helpers.*`), or a FAIL if Task 4's helper skipped a no-coverage shard. The "no prior coverage ⇒ Read" case should already pass from Task 4's `cov != nullptr` guard; if so, this is a passing characterization test that locks the behavior — keep it.

- [ ] **Step 3: Harden `computeDiscoverDecisions` for ambiguity.** In `CasGc.cpp`, make the LIST sweep detect a duplicate `rootShardKey` (treat a key seen twice as ambiguous ⇒ Read) and keep the explicit fail-closed default. Replace the per-shard decision body with:

```cpp
        // listRootShardTokens marks a key AMBIGUOUS when it appears more than once in the sweep.
        std::map<String, Token> listed_token;
        std::set<String> ambiguous_keys;
        if (token_observable)
            listed_token = listRootShardTokens(ambiguous_keys);   /// fills ambiguous_keys on duplicates

        for (const auto & [ns, shard] : universe)
        {
            DiscoverDecision decision = DiscoverDecision::Read;   /// fail closed by default
            if (token_observable)
            {
                const String key = store->layout().rootShardKey(ns, shard);
                const auto it = listed_token.find(key);
                const ShardCoverage * cov = sealed.find(ns, shard);
                const bool unambiguous = ambiguous_keys.find(key) == ambiguous_keys.end();
                if (unambiguous && it != listed_token.end() && cov != nullptr
                    && tokensEqual(it->second, cov->folded_token))
                {
                    decision = DiscoverDecision::Skip;
                }
            }
            out[{ns, shard}] = decision;
        }
```

- [ ] **Step 4: Run the tests to verify they pass.**

Run: `cd build && ninja unit_tests_dbms > build_cas_phase2_t5b.log 2>&1` then `build/src/unit_tests_dbms --gtest_filter='CasGcDiscovery*' > test_cas_phase2_t5c.log 2>&1`. Analyze both logs with a subagent.
Expected: every `CasGcDiscovery.*` test PASSES, including the two fail-closed cases.

- [ ] **Step 5: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/cas_test_helpers.h \
        src/Disks/tests/cas_test_helpers.cpp \
        src/Disks/tests/gtest_cas_gc_token_diff.cpp
git commit -m "CA GC phase2: fail-closed discover fallback to body reads on missing/ambiguous/unobservable tokens"
```

(Drop the `cas_test_helpers.*` paths from `git add` if Step 1 needed no new helper.)

---

### Task 6: Build + full `Cas*:Ca*` sweep + phase-exit commit {#task-6-build-full-cas-ca-sweep-phase-exit-commit}

**Files:**
- No source changes (verification + phase-exit ledger only).

**Interfaces:**
- Consumes: every artifact of Tasks 1–5.
- Produces: a green phase-exit state — the build is clean and the full `Cas*:Ca*` gtest sweep passes (only the baseline-red `CaWiringOps.FreezeViaHardLinksIntoShadow` tolerated), confirming Phase 2's discovery laziness is behavior-preserving.

- [ ] **Step 1: Clean build of the unit-test target.**

Run: `cd build && cmake . > build_cas_phase2_cmake.log 2>&1 && ninja unit_tests_dbms > build_cas_phase2_final.log 2>&1` (no `-j`, no `nproc`). **Analyze `build/build_cas_phase2_final.log` with a subagent and return only a concise summary.**
Expected: build succeeds, no errors.

- [ ] **Step 2: Run the full CA gtest sweep.**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > test_cas_phase2_sweep.log 2>&1`. **Analyze `build/test_cas_phase2_sweep.log` with a subagent**; return a concise pass/fail summary.
Expected: all PASS except the single tolerated baseline-red `CaWiringOps.FreezeViaHardLinksIntoShadow`. Any other red is a Phase 2 regression — fix it (return to the offending task) before continuing.

- [ ] **Step 3: Re-confirm the TLA+ gate is still green** (the model file changed in Task 1; nothing since should affect it, but confirm before declaring the phase done).

Run:
```bash
cd docs/superpowers/models
TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage5_tokendiff
TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_skipchangedshard && echo "UNEXPECTED PASS"
```
Expected: `stage5_tokendiff` prints `No error` (exit 0); `sab_skipchangedshard` prints `Error: Invariant INV_NO_DANGLE is violated.` (nonzero exit) and **no** `UNEXPECTED PASS`.

- [ ] **Step 4: Commit the phase-exit marker.**

```bash
git commit --allow-empty -m "CA GC phase2: token-diff discovery complete — gate green, full Cas*:Ca* sweep passes"
```

---

## Self-Review {#self-review}

- **Spec coverage (spec §Phase 2 four bullets):**
  1. *Add LIST token capability probes* → Task 2 (`Backend::supportsListTokens`). ✓
  2. *Persist folded root shard state tokens with folded cursors* → Task 3 (`ShardCoverage.folded_token`/`folded_cursor` round-trip). ✓
  3. *Skip unchanged root shards only when LIST token freshness is proved* → Task 1 (model gate) + Task 4 (`discover` skip iff listed token == folded token). ✓
  4. *Keep fail-closed fallback to body reads on ambiguous/missing tokens* → Task 5 (missing/ambiguous/unobservable/no-coverage ⇒ Read). ✓
  - §Discovery rule (listed==folded ⇒ skip; missing/ambiguous/stale/unsupported ⇒ read; LIST never shrinks the universe; fail closed) → Task 4 helper + Task 5 hardening + `CasGcDiscovery.RegistryUniverseNeverShrunkByList`. ✓
  - Gate definition matches the overview (positive stage HOLDs; `_sab_*` VIOLATES; no `UNEXPECTED PASS`) → Task 1 Steps 9/11/12. ✓
- **Placeholder scan:** every code step shows real code; every run step shows the exact command + expected output; the only "confirm from the Phase 1d report" notes are about *names produced by a dependency phase* (legitimate — Phase 1d owns `ShardCoverage`'s exact field/enum/codec spelling), and each carries the consuming intent so the implementer can reconcile. No TBD/TODO/"handle edge cases". ✓
- **Type consistency:** `DiscoverDecision{Skip, Read}` and `discoverDecisionsForTest` are defined in Task 4 and reused unchanged in Tasks 5/6; `supportsListTokens` is defined in Task 2 and consumed in Tasks 4/5; `ShardCoverage.folded_token`/`folded_cursor` defined/round-tripped in Task 3 and consumed in Task 4; `computeDiscoverDecisions` defined in Task 4 and hardened in Task 5; the model's `EnableTokenDiff`/`TokenObservable`/`SabotageSkipChangedShard`/`foldedTok`/`rootTok`/`GDiscoverSkip`/`GDiscoverRead` are introduced in Task 1 only. ✓
- **Contract discipline:** only Phase-1d canonical type names are used (`GenerationSeal`, `ShardCoverage{classification, folded_token, folded_cursor}`, `CasGc` round, `CasLayout::generationSealKey`, `CasBlobInDegree`); no invented sibling abstractions. ✓
- **Dependency & gate ordering:** Task 1 (model) gates Tasks 2–6; Task 6 re-confirms the gate at phase exit; the plan depends on Phase 1d and consumes its contract verbatim. ✓
