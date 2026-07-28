# CAS ref contiguous streams — Stage A (streams) Implementation Plan

> **DRAFT MARKER: being elaborated — do not execute until this line is removed.**

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the LIST-incompleteness release blocker for existing namespaces: per-namespace
contiguous ref ids, the in-band `EpochSeal`, `_ckpt`, arithmetic fold + recovery, the
destructive-round frontier proof and REBUILD-surviving holds — with LIST demoted to a zero-trust
hint on every consumer this stage touches.

**Architecture:** Spec `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md`
(v9 CONVERGED), INV-1/INV-2/INV-4 + §4 recovery + §5 fold/holds/frontier + §6 sweep premise +
§7 REBUILD/fsck. Stage B (catalog + incarnations, INV-3) is a separate plan
(`2026-07-28-cas-ref-chain-stage-b-catalog.md`) with its own soak gate; this stage is fully
functional without it. TLA gate: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` says
`TLA PHASE: PASS` (93/93) — models are normative for every mechanism named below.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`),
gtest (CA gate filter), integration lanes (`with_rustfs`), `utils/ca-soak` (phase 3 `--duration`),
TLA+ models under `docs/superpowers/models/` (already green; extended only by register items).

## Global Constraints {#global-constraints}

Copied from the spec, the project instructions and the standing user directives — every task's
requirements implicitly include ALL of these:

1. Branch `cas-gc-rebuild`; new commits only (no rebase/amend); **NEVER `git push`**.
2. Allman braces; never `sleep` to fix a race; wrap literal names in backticks in docs/comments;
   say "exception" not "crash" for logical errors; ASan not ASAN.
3. **Fail-close, no fallback paths**: when an operation fails, propagate; never substitute a
   default. A defensive branch whose premise Stage A kills is REMOVED and replaced by a loud
   check (see Task 12), never left "just in case".
4. **Recreate-only migration, zero compat scaffolding** (pre-release): Stage A bumps the pool
   format writer generation AND backward floor once (Task 3); old-format open fails closed naming
   pool recreation; no dual-format readers, no upgrade converters.
5. CAS identity primitives are untouchable: never GET a condemned object to revive it (revival =
   fresh re-upload); no skip-read hash-equality shortcuts (re-hashing is the identity primitive);
   the HEAD-before-PUT dedup protocol steps are not "optimization" targets.
6. No CA-specific fields or hooks in generic Replicated/Keeper code or formats; changes to
   shared/upstream surfaces require consultation BEFORE editing.
7. Wire formats changed here (`CasRefLogFormat`, `CasFoldSealFormat`, `CasRefSnapshotFormat`)
   follow the strict-grammar rule: fields required in exactly the states that permit them,
   forbidden otherwise, rejected loudly on violation; every codec change lands with
   boundary AND boundary-plus-one tests (spec r9-4).
8. The append hot path gains **+0 requests**; recovery/seal/`_ckpt` costs are per spec §8 and a
   task exceeding them is out of spec (raise, do not land).
9. Any register item (R2/R3/…) landed later extends its TLA model per spec §9 — Stage A tasks do
   NOT edit the five phase models; model edits in Stage A are forbidden (the gate stays sealed).
10. Every new externally-visible failure mode gets a `LOG_WARNING`-or-stronger message naming the
    object key and the decision taken (the CI-observability lesson: unlogged decisions block
    triage).
11. Tests: prefer NEW test files over extending existing ones; stateless tests via
    `./tests/queries/0_stateless/add-test`; no `no-*` tags unless strictly necessary; gtests run
    under the CA gate filter (exact filter recorded in Task 0).

## Staging contract {#staging-contract}

**Stage A delivers standalone:** contiguous ids + seals + `_ckpt` at TODAY's un-qualified key
shape (`<ns>/_log/...`, `<ns>/_snap/...`, `<ns>/_ckpt`); namespace DISCOVERY still uses today's
mechanisms (hint enumeration + `gc/state` cursors) — but every per-namespace decision (fold
advance, deletion safety, recovery completeness) becomes arithmetic/point-read/CAS.

**Named Stage-A residuals (closed by Stage B, honest until then):**
- A namespace absent from BOTH the hint and `gc/state` has no frontier entry (the universe is not
  yet authoritative — INV-3's job). New-namespace creation is DDL-rate; the observed blocker class
  (existing-namespace hidden records) is closed by Stage A.
- Verbatim-file rebirth aliasing (register R1) — pre-existing, untouched here.
- Manifest-less orphan blobs after REBUILD goes condemn-nothing (register R4) — a NAMED leak, not
  a regression: today's REBUILD condemnation of acked data is the thing being removed.

**Interface handed to Stage B:** the `EpochSeal`/`_ckpt` codecs and the slot-occupy primitive are
namespace-shape-agnostic (they take full keys); Stage B re-keys the layer under `<ns>/<inc>/` and
swaps discovery to the catalog without touching Stage A's arithmetic.

---

## Task overview {#task-overview}

| # | Task | Spec | Depends on |
|---|---|---|---|
| 0 | Gate + baseline preflight | §9 | — |
| 1 | `EpochSeal` record kind + grammar | INV-2 | 0 |
| 2 | Slot-occupy backend primitive | INV-2 | 0 |
| 3 | Contiguous allocator; delete `next_ref_sequence`; format bump | INV-1 | 0 |
| 4 | Writer wedge: every-attempt rule + admission fence | INV-1/INV-2 | 1,2,3 |
| 5 | `_ckpt` object + shared semantic-max merge + fence recheck helper | INV-4 | 0 |
| 6 | Recovery CAS-walk + install-lock generation recheck; retire sentinel seal | §4 | 1,2,4,5 |
| 7 | Fold arithmetic intake; seals cross epochs; B1/B2 accounting | §5 | 1,3 |
| 8 | Holds: classification-4 grammar, REBUILD carry, refuse-on-missing-seal | §5/§7 | 7 |
| 9 | Frontier proof + `suppress_destructive` before every destructive site | §5 | 7,8 |
| 10 | Sweep §6 deletion premise (two rules; retain on uncertainty) | §6 | 7,9 |
| 11 | REBUILD condemn-nothing; fsck arithmetic streams | §7 | 7,8 |
| 12 | Retirement sweep (probe A demotion; grace/gap-heuristic/reconciliation verdicts) | §5/R7 + ledger | 6,7,9 |
| 13 | LIST-liar fault injection + end-to-end blocker regression test | §1/§9 | 4,6,7,9 |
| 14 | Stage A gates: gtest + integration + soak; stage summary + PASS line | §9 | all |

Tasks 1, 2, 3, 5 are independent after 0 — but per SDD discipline they are still executed
SEQUENTIALLY (never two implementers in one worktree).

---

### Task 0: Gate + baseline preflight {#task-0}

**Files:**
- Read: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md`
- Create: nothing (evidence goes into the task report)

**Interfaces:**
- Produces: the recorded BASELINE facts every later task's report compares against.

- [ ] **Step 1:** Verify the TLA gate: `grep -n "TLA PHASE: PASS" docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` — must match exactly once. If absent, STOP: report BLOCKED (the phase gate is a precondition of this whole plan).
- [ ] **Step 2:** Build the current tree (`ninja` in an existing `build*` dir, output redirected to `<build>/task0_build.log`) and run the CA gtest gate, output to `<build>/task0_gtest.log`:
  `<build>/src/unit_tests_dbms --gtest_filter='Cas*:CaLifecycle*:CaWiring*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'`
  Expected: all tests pass (~1246 tests / 228 suites as of the phase close). Record the exact counts in the report — every later task re-runs this same filter and must not regress the count downward except by explicitly-reported test replacement.
- [ ] **Step 3:** Record in the report: current `git rev-parse HEAD`, gtest counts, build dir used. No commit (nothing changed).

### Task 1: `EpochSeal` record kind + grammar {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h` (enum `:24-30`, caps block, struct fields)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp` (`opKindToWord` `:23`, `opKindFromWord` `:35`, encode switch `:95-104`, `readOpRecord` `:170` + switch `:214-223`, `refLogTxnIsRemovalClass` `:338`)
- Create: `src/Disks/tests/gtest_cas_ref_epoch_seal_format.cpp`

**Interfaces:**
- Consumes: existing `RefOpKind`, txn meta encode/decode, `RefTxnId`.
- Produces (verbatim names later tasks use):
  - `RefOpKind::EpochSeal = 5`
  - txn-meta field `std::optional<RefTxnId> prev_epoch_seal;` on the decoded txn struct
  - helpers `bool refLogTxnIsEpochSeal(const RefLogTxn &)` and the grammar validator
    `void validateEpochSealGrammar(const RefLogTxn &)` (throws `CORRUPTED_DATA`)

Grammar (spec INV-2, verbatim rules): a seal txn contains EXACTLY ONE op and it is `EpochSeal`;
`prev_epoch_seal` is REQUIRED on exactly sequence 1 of every non-genesis epoch (including a
sequence-1 seal closing an empty epoch) and FORBIDDEN elsewhere; the seal's own id is
`{closing_writer_epoch, T+1}` where `T` = greatest applied sequence of the epoch being closed
(so an empty dead epoch is closed by a seal at sequence 1 carrying `prev_epoch_seal`).

- [ ] **Step 1: Write the failing codec tests** in `gtest_cas_ref_epoch_seal_format.cpp`:
  round-trip of a seal txn (encode → decode → fields equal, incl. `prev_epoch_seal`);
  strict rejections, one test each, all expecting `CORRUPTED_DATA`:
  seal txn with 2 ops; seal txn with a second non-seal op; non-seal txn carrying
  `prev_epoch_seal` at sequence 2; sequence-1 txn of a non-genesis epoch WITHOUT
  `prev_epoch_seal`; genesis-epoch sequence-1 txn WITH `prev_epoch_seal`; unknown op word
  (existing behavior still intact — regression guard).
- [ ] **Step 2:** Run: `unit_tests_dbms --gtest_filter='CasRefEpochSealFormat*'` → FAIL (kind not defined).
- [ ] **Step 3:** Implement: add the enum value + word mapping (`"epoch_seal"`), the optional
  meta field (additive encode: emit only when set; decode: accept once, reject duplicates),
  `validateEpochSealGrammar` called from `readOpRecord`'s txn finalization AND from the encoder
  (both directions fail closed — Constraint 7). `refLogTxnIsRemovalClass` returns `false` for seals.
- [ ] **Step 4:** Run the new filter → PASS; run the full CA gate filter → no regressions.
- [ ] **Step 5: Commit** `ca: formats — EpochSeal record kind + strict seal grammar`.

### Task 2: Slot-occupy backend primitive {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h` (near `putIfAbsentControlled` `:365` / `conditionalCreateControlled` `:407`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp`
- Create: `src/Disks/tests/gtest_cas_slot_occupy.cpp`

**Interfaces:**
- Consumes: `putIfAbsentControlled` (conflict returns NO bytes — r9-6), `resolveByExactGet` `:377`, `CasUnresolvedReason`.
- Produces (the primitive every seal writer and wedge retry uses — spec INV-2):
```cpp
struct SlotOccupyResult
{
    enum class Kind : uint8_t { Created, Occupied, Unresolved };
    Kind kind = Kind::Unresolved;
    /// Occupied only: the occupant, fetched by exact GET after the conditional create conflicted.
    String occupant_bytes;
    Token occupant_token;
    /// Unresolved only: why the attempt outcome is unknowable right now.
    CasUnresolvedReason unresolved_reason{};
};

/// One conditional create of `bytes` at `key`; on conflict, one exact GET of the occupant.
/// NEVER retries internally; NEVER lists. fence_ok is re-evaluated by the controlled layer
/// per attempt (admission fence discipline is the caller's contract, Task 6).
SlotOccupyResult slotOccupy(std::string_view key, std::string_view bytes,
                            const std::function<bool()> & fence_ok);
```
  Adjudication is the CALLER's job (byte-compare — the `CaCasMountCore` `mine` contract: an
  occupant is "my write" only if the BYTES match my attempt; a generation/shape match alone is
  the aliasing bug the model killed). The primitive only guarantees: `Occupied` always carries
  the occupant's bytes+token; a GET that finds nothing after a conflict (occupant vanished
  mid-resolve) returns `Unresolved`, never a fabricated `Created`.

- [ ] **Step 1: Failing tests** in `gtest_cas_slot_occupy.cpp` over `InMemoryBackend`
  (`Backend/CasInMemoryBackend.cpp` `putIfAbsent` `:131` contract):
  absent key → `Created`; pre-existing key → `Occupied` with exact bytes+token;
  injected ambiguous PUT (fault hook, see Step 3) → `Unresolved` with reason;
  conflict-then-vanish (delete between conflict and GET) → `Unresolved`;
  fence flip mid-call → the controlled layer's refusal surfaces as `Unresolved`
  with the pre-attempt reason (never a lie of `Created`).
- [ ] **Step 2:** Run `--gtest_filter='CasSlotOccupy*'` → FAIL (symbol undefined).
- [ ] **Step 3:** Implement `slotOccupy` in `CasRequestControl.cpp` composing
  `putIfAbsentControlled` + on `PreconditionFailed`/`Conflict` one `resolveByExactGet`.
  Extend `InMemoryBackend` with the minimal fault hook the tests need (an injected
  "ambiguous outcome" toggle for `putIfAbsent`) if one does not already exist — test-only
  surface, Allman braces, no sleeps.
- [ ] **Step 4:** New filter PASS + full CA gate no regressions.
- [ ] **Step 5: Commit** `ca: backend — slotOccupy primitive (Created|Occupied(bytes)|Unresolved)`.

### Task 3: Contiguous allocator — delete `next_ref_sequence`, format bump {#task-3}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` (`:535` declaration, `:541` `allocateRefTxnId`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` (`:1940` sole caller; trial-preview seeding `:1671-1677` stays as the model)
- Modify: the pool-format version constants + open-path check (locate via the existing writer-generation/backward-floor constants in `Pool/` — the Task-0 build tree makes `grep -rn "backward" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/` unambiguous)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:433` (strict-increase check becomes contiguity check)
- Create: `src/Disks/tests/gtest_cas_ref_contiguous_alloc.cpp`

**Interfaces:**
- Consumes: `RefTableRuntime` per-namespace state (`getGreatestApplied()`), `live_epoch_fn`.
- Produces: `RefTxnId CasRefLedger::allocateRefTxnId(const RefTableRuntime & rt)` — PER-NAMESPACE,
  state-derived: `{E, greatest_applied.ref_sequence + 1}` when `greatest_applied.writer_epoch == E`,
  else `{E, 1}`. The pool-wide `std::atomic<uint64_t> next_ref_sequence` (`CasRefLedger.h:535`) is
  DELETED — both references enumerated by the site map are in these two lines; there are no other
  code consumers in the tree.

Semantics change carried by this task (INV-1): within `(namespace, epoch)` durable ids are dense
`1..T`. `CasRefProtocol.cpp:433` (today: strict increase only) now REJECTS a non-successor id on
apply (`CORRUPTED_DATA` naming both ids) — the read side enforces density; the comment at
`CasRefLedger.cpp:2189-2190` ("ids need not be contiguous") is deleted with the behavior.
Allocation failure paths that today leave "safe gaps" (`:1904`, `:1932`, `:1973`, `:1997`,
`:2027`, `:2148`, `:2193`) now roll the id back trivially — the id was never anything but
`greatest_applied+1`, so a failed attempt that provably sent nothing (`NoAttemptSent`) leaves
state untouched and the NEXT caller re-derives the same id (the every-attempt rule's free half;
the wedged half is Task 4).

- [ ] **Step 1: Failing tests** in `gtest_cas_ref_contiguous_alloc.cpp`:
  two namespaces allocate independently (`ns_a` gets 1,2,3 while `ns_b` gets 1,2 — today FAILS
  because the pool-wide counter interleaves 1,2,4 / 3,5); a definite pre-attempt refusal
  (`NoAttemptSent`) followed by a successful commit yields the SAME id (no gap — today FAILS,
  gap by design); epoch change resets to 1; apply of `{E, 3}` onto greatest `{E, 1}` throws
  `CORRUPTED_DATA` (density — today FAILS, strict-increase admits it).
- [ ] **Step 2:** Run `--gtest_filter='CasRefContiguousAlloc*'` → FAIL as annotated.
- [ ] **Step 3:** Implement the allocator + protocol check + delete the atomic. Bump the pool
  format WRITER GENERATION and BACKWARD FLOOR together (one bump for all of Stage A — later
  Stage-A tasks change semantics under the same version); the old-format open path must fail
  closed with an exception message naming pool recreation as the migration
  (`"CAS pool format <old> predates contiguous ref streams; recreate the pool"` — exact message in
  the test).
- [ ] **Step 4:** New filter PASS; full CA gate: expect and FIX fallout in tests that relied on
  pool-wide monotonicity across namespaces (report each such edit explicitly — reviewer checks
  the list against `git diff --stat`).
- [ ] **Step 5: Commit** `ca: ref — per-namespace contiguous ids; delete next_ref_sequence; format floor bump`.

### Task 4: Writer wedge — every-attempt rule + admission fence {#task-4}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` (`commitRefChunk` `:1873-2243`; wedge struct + install region 3 `:2201-2235`; pre-attempt refusal path `:2177-2199`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` (the `RefAppendWedge` struct — add the fence generation + attempt identity)
- Create: `src/Disks/tests/gtest_cas_ref_wedge_every_attempt.cpp`

**Interfaces:**
- Consumes: Task 2's `slotOccupy`; Task 1's `refLogTxnIsEpochSeal`; `CasMountRuntime::fenceGeneration()` (`Pool/CasMountRuntime.h:143`) captured at admission (pattern: `ContentAddressedTransaction.cpp:887`).
- Produces (Task 6 consumes): `RefAppendWedge` carrying
  `{key, bytes, RefTxnId id, uint64_t admitted_fence_generation}`; the lane rule
  "at most one in-flight PUT per namespace lane"; `resolveWedgeOnce(rt)` — the bounded retry a
  later caller's flush performs.

Behavior (spec INV-1/INV-2 verbatim):
- An id is freed only when nothing was sent (`unresolvedProvesNothingWasSent`, today's `:2177`
  branch — KEPT, now actually re-deriving the same id per Task 3) or every sent attempt has its
  own conclusive rejection. A definite rejection AFTER an ambiguous attempt keeps the lane
  wedged (the ambiguous attempt may still land — this is the model-proven
  `ambiguous-then-definite` control).
- `resolveWedgeOnce`: at most ONE `slotOccupy(key, wedge.bytes, fence_ok)` per later caller's
  flush, under the wedge's ORIGINAL `admitted_fence_generation` — never the current one
  (`CaCasMountCore` `WedgeRetryCreate` semantics). Outcomes:
  `Created` → our operation is durable → adopt as committed (fold it exactly like region 1);
  `Occupied` + bytes == wedge.bytes → an earlier attempt landed → same adoption;
  `Occupied` + `refLogTxnIsEpochSeal(decode(bytes))` → CONCLUSIVE REJECTION by a successor's
  seal → clear the wedge, fail the survivors with the permanent error (op was never acked),
  lane resumes at the seal's successor id;
  `Occupied` + anything else → `CORRUPTED_DATA` (same-epoch foreign record is impossible —
  fail loud, Constraint 3);
  `Unresolved` → stay wedged (no deadline reset, no background loop — register R6 is ACCEPTED
  behavior: a permanently quiet wedged namespace retries on its next caller or remount).
- NO background retry thread exists or is added.

- [ ] **Step 1: Failing tests** in `gtest_cas_ref_wedge_every_attempt.cpp` (InMemoryBackend +
  fault hooks from Task 2):
  ambiguous PUT wedges the lane, next flush's `resolveWedgeOnce` with `Created` adopts and the
  ops land exactly once; ambiguous PUT, then occupant appears with OUR bytes → adopt (no
  double-apply — assert applied txn count); ambiguous, then a seal occupies the key → survivors
  fail permanently, wedge cleared, next allocation = seal id + 1 in the new epoch's terms;
  ambiguous PUT then a DEFINITE refusal of a retry attempt → lane STAYS wedged
  (`ambiguous-then-definite`); occupant with foreign non-seal bytes → throws `CORRUPTED_DATA`;
  fence generation bumped between wedge creation and retry → retry refused pre-attempt
  (`Unresolved`, wedge intact) — the old-generation-retry-inert rule
  (`_sab_wedgeretryoldgen`'s C++ twin).
- [ ] **Step 2:** Run `--gtest_filter='CasRefWedgeEveryAttempt*'` → FAIL.
- [ ] **Step 3:** Implement. The wedge hard-contract `chassert` at `:1918-1938` stays; extend it
  to also refuse NEW allocations while a wedge is unresolved (the lane rule — one in-flight PUT
  per namespace lane, and the allocator's `greatest_applied+1` would collide with the wedged id).
- [ ] **Step 4:** New filter PASS + full CA gate (expect fallout in `gtest_cas_ref_writer.cpp` /
  `gtest_cas_ref_chunked_flush.cpp` around gap expectations — fix and report each).
- [ ] **Step 5: Commit** `ca: ref — every-attempt wedge with admission fence; seal is a conclusive rejection`.


