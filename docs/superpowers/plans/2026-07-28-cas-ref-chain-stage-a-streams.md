# CAS ref contiguous streams — Stage A (streams) Implementation Plan

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

**Stage A is NOT deletion-capable — destruction is globally suppressed until Stage B.**
Blob in-degree is POOL-WIDE (spec §1): an acked hidden `+1` in a namespace absent from both the
hint and `gc/state` can coexist with a visible `-1` elsewhere, and no per-known-namespace
frontier proof can exclude it — the catalog universe (Stage B Task 4) is LOAD-BEARING for the
acked-loss closure. Therefore Stage A hard-wires `frontier_incomplete = true` at the universe
seam (`UniversePolicy::kDefault = StageA_Suppressed`, a source-level default with a comment citing
this paragraph),
so `suppress_destructive` holds on EVERY round: folds, seals, cursors, holds, recovery and
`_ckpt` all run for real; every delete family stays inert. Stage B flips the constant when the
catalog becomes the universe. Deletion-path CORRECTNESS is still fully tested in Stage A gtests
(tests construct closed universes where the constant is overridden through the test seam); only
production destruction waits. [Codex plans-review finding 1 — the original "new-namespace
residual" argument was FALSE and is retired.]

**Named Stage-A residuals (closed by Stage B, honest until then):**
- No production destruction (above) — GC debris accumulates until Stage B lands; the soak gate's
  criteria account for this (Task 14).
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
  - helpers `bool refLogTxnIsEpochSeal(const RefLogTxn &)` and the grammar validators, SPLIT
    by what context they need [codex r3 finding 2]:
    `void validateEpochSealGrammarStructural(const RefLogTxn &)` — the context-free rules
    (exactly-one-op seal txn; `prev_epoch_seal` well-formed and at sequence 1 only), called by
    BOTH `encodeRefLogTxn` and the decode path (`readOpRecord` finalization);
    `void validateEpochSealGrammarContextual(const RefLogTxn &, uint64_t life_epoch)` — the
    required-iff rule (`prev_epoch_seal` present iff `writer_epoch > life_epoch`), called where
    `life_epoch` is KNOWN: the apply layer (`CasRefProtocol` — Task 3 wires it) and the two
    encode call sites (`commitRefChunk` Task 4, recovery sealer Task 6). The codec itself never
    sees `life_epoch` — its callers own the contextual check. Both throw `CORRUPTED_DATA`.

Grammar (spec INV-2, with the genesis contract made explicit — codex r2 finding 2): "genesis"
is PER-NAMESPACE — the namespace's `life_epoch` (the writer epoch of its `NamespaceBirth`
record; a brand-new namespace born at global epoch `E > 1` is at ITS genesis). Validation is SPLIT (see Produces): structural rules live in the codec; the required-iff rule
is contextual — `validateEpochSealGrammarContextual(const RefLogTxn &, uint64_t life_epoch)` —
and runs where `life_epoch` is known. Rules: a seal txn contains EXACTLY ONE op and it is `EpochSeal`; `prev_epoch_seal` is REQUIRED
on exactly sequence 1 of every epoch with `writer_epoch > life_epoch` (including a sequence-1
seal closing an empty epoch) and FORBIDDEN elsewhere — in particular FORBIDDEN at
`{life_epoch, 1}`; the seal's own id is `{closing_writer_epoch, T+1}` where `T` = greatest
applied sequence of the epoch being closed (an empty dead epoch closes with a sequence-1 seal
carrying `prev_epoch_seal`).

**The field is populated by BOTH writers** [codex finding 2 — blocker]: recovery seals (Task 6)
AND every ORDINARY `{E, 1}` transaction a writer appends after an epoch transition. Recovery
therefore installs the exact last-seal id into the writer runtime
(`RefTableRuntime::last_epoch_seal`, set by `installRecoveryResult` — Task 6 produces it), and
`commitRefChunk` copies it into every non-genesis sequence-1 txn it encodes (Task 4 wires
this). Without that, the first normal append after any transition would fail this task's own
encoder grammar. This task defines the field + validator; the round-trip test for the ordinary
path lands in Task 4 Step 1 (it needs the writer runtime).

- [ ] **Step 1: Write the failing codec tests** in `gtest_cas_ref_epoch_seal_format.cpp`:
  round-trip of a seal txn (encode → decode → fields equal, incl. `prev_epoch_seal`);
  strict rejections, one test each, all expecting `CORRUPTED_DATA`:
  seal txn with 2 ops; seal txn with a second non-seal op; non-seal txn carrying
  `prev_epoch_seal` at sequence 2; sequence-1 txn with `writer_epoch > life_epoch` WITHOUT
  `prev_epoch_seal`; sequence-1 txn AT `life_epoch` WITH `prev_epoch_seal` (both directions of
  the contextual rule); `life_epoch > 1` birth: `{life_epoch = 5, seq 1}` without the field is
  VALID (the r2-finding-2 case); unknown op word (existing behavior still intact — regression
  guard).
- [ ] **Step 2:** Run: `unit_tests_dbms --gtest_filter='CasRefEpochSealFormat*'` → FAIL (kind not defined).
- [ ] **Step 3:** Implement: add the enum value + word mapping (`"epoch_seal"`), the optional
  meta field (additive encode: emit only when set; decode: accept once, reject duplicates),
  `validateEpochSealGrammarStructural` called from `readOpRecord`'s txn finalization AND from
  the encoder (both directions fail closed — Constraint 7); `validateEpochSealGrammarContextual`
  exported for the apply/encode call sites (wired by Tasks 3/4/6 — the codec never sees
  `life_epoch`). `refLogTxnIsRemovalClass` returns `false` for seals.
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
- [ ] **Step 3:** Implement `slotOccupy` in `CasRequestControl.cpp` as a DEDICATED RAW slot
  operation [codex finding 3 — the naive composition is wrong: `putIfAbsentControlled` retries
  internally and `resolveByExactGet` compares-against-expected and throws `CORRUPTED_DATA` on a
  different occupant, contradicting both "one conditional create, never retries internally" and
  `Occupied(bytes, token)`]: exactly ONE fence/deadline-gated backend `putIfAbsent`, then — only
  when resolution is required — exactly ONE raw exact `GET` returning the occupant's bytes and
  token WITHOUT compare-and-throw (adjudication is the caller's, per the Interfaces block). Add
  backend operation-count assertions to every Step-1 test (Created = 1 op; Occupied = 2 ops;
  Unresolved ≤ 2 ops). Extend `InMemoryBackend` with the minimal fault hook the tests need (an
  injected "ambiguous outcome" toggle for `putIfAbsent`) if one does not already exist —
  test-only surface, Allman braces, no sleeps.
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
- [ ] **Step 3b: Recreation quiesce enforcement** [codex finding 11 — rejecting old-format
  OPEN does not fence an already-RUNNING old binary that keeps writing the reused prefix]. The
  spec's migration rule ("recreation must be quiesced so no old writer touches the reused
  prefix") gets teeth: pool recreation onto a prefix with a live mount lease of ANY format
  generation REFUSES until the existing owner slots are terminal (the mount-claim machinery
  already knows how to read them — reuse it read-only), and the recreated pool's first mount
  fences via the normal claim path so a surviving old writer's next controlled write dies on
  the fence. Integration test (goes into the Task 14 battery, defined here): old-binary-shaped
  writer holds the lease → recreation attempt refused; owner terminalized → recreation
  proceeds → a delayed write from the old writer's queue is refused by the fence. Stage A
  cannot take `STAGE A: PASS` without this test green.
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
- **Post-I/O recheck under the install lock** [codex finding 8 — the r9-5 rule applies to the
  wedge, not only to recovery]: the outcome adjudication above happens on I/O results; BEFORE
  acting on them (adopt / acknowledge / unwedge / fail-survivors), re-acquire `state_mutex` and
  compare the exact stored `(admitted_fence_generation, wedge txn identity, wedge bytes)`
  against the wedge still installed AND `checkFenceOrThrow(admitted_fence_generation)` — a
  result returning after a fence bump/rearm or a successor's seal must be INERT for the
  superseded runtime (the successor's seal remains the conclusive rejection of the OPERATION;
  the superseded runtime just may not be the one to act on it).
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
  (`_sab_wedgeretryoldgen`'s C++ twin); **deterministic blocked-I/O race** [codex finding 8]:
  retry's `slotOccupy` blocked at the fault seam → fence bumped + rearmed and a successor seals
  the slot → I/O released with a stale result → the post-I/O recheck makes the superseded
  runtime act on NOTHING (no ack, no unwedge, no install — assert all three); ordinary
  `{E, 1}` append after a sealed transition carries the exact `prev_epoch_seal` and round-trips
  (the blocker-2 test, here because it needs the writer runtime) [codex finding 2]; GENESIS
  INITIALIZATION [codex r2 finding 2]: `RefTableRuntime::last_epoch_seal` is `none` exactly for
  a namespace whose recovered state contains no seal and whose `greatest_applied.writer_epoch ==
  life_epoch` — first birth at global epoch 5 appends `{5, 1}` with NO `prev_epoch_seal`; that
  namespace's FIRST transition (5 → 6) then seals `{5, T+1}` and the `{6, 1}` append carries it
  (both tested).
- [ ] **Step 2:** Run `--gtest_filter='CasRefWedgeEveryAttempt*'` → FAIL.
- [ ] **Step 3:** Implement. The wedge hard-contract `chassert` at `:1918-1938` stays; extend it
  to also refuse NEW allocations while a wedge is unresolved (the lane rule — one in-flight PUT
  per namespace lane, and the allocator's `greatest_applied+1` would collide with the wedged id).
- [ ] **Step 4:** New filter PASS + full CA gate (expect fallout in `gtest_cas_ref_writer.cpp` /
  `gtest_cas_ref_chunked_flush.cpp` around gap expectations — fix and report each).
- [ ] **Step 5: Commit** `ca: ref — every-attempt wedge with admission fence; seal is a conclusive rejection`.

### Task 5: `_ckpt` object — shared semantic-max merge + fence-recheck discipline {#task-5}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h` / `.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp` (registry — add `FormatId::RefCkpt`, Control/Strict, modest caps: 64 KiB object / 4 KiB line)
- Modify: `.../ContentAddressed/Formats/CasLayout.h` — the `Layout` class (+ member
  `String refCkptKey(const RootNamespace &) const` — Stage A shape `<ns>/_ckpt`; Stage B
  re-keys under the incarnation) [path per codex finding 18]
- Create: `.../ContentAddressed/Pool/CasRefCkpt.h` / `.cpp` (the write algorithm)
- Modify: `.../ContentAddressed/Pool/CasRefLedger.cpp` — `trySnapshotPublishOnce` (the snapshot
  publisher is INV-4's SECOND writer [codex finding 4]: after the snapshot body PUT commits and
  BEFORE the snapshot is treated as cleanup-authoritative, it calls the same `publishCkpt` with
  `checkpoint_snapshot_id = <published id>`), + the constructor/callback plumbing for the
  generation capture the call needs (the ledger does not own a `CasMountRuntime` — plumb a
  `fence_generation_fn`/`check_fence_fn` pair the way `CasPlainObjects` does via `CasPool.cpp:171`)
- Create: `src/Disks/tests/gtest_cas_ref_ckpt.cpp`

**Interfaces:**
- Consumes: `casPut` (token-CAS, `Backend/CasBackend.h:250`), `checkFenceOrThrow` (`Pool/CasMountRuntime.h:151`).
- Produces (Tasks 6/8/9 consume — names verbatim):
```cpp
struct RefCkpt
{
    uint64_t life_epoch = 0;                      /// namespace birth epoch (Stage A: from NamespaceBirth)
    std::optional<RefTxnId> checkpoint_snapshot_id;
    std::optional<RefTxnId> last_epoch_seal;
};

/// THE one merge — both writers use it; per-field semantic maximum (spec INV-4).
RefCkpt mergeCkpt(const RefCkpt & a, const RefCkpt & b);

enum class CkptPublishOutcome : uint8_t { Published, IdenticalSkip, FencedOut };
/// read → validate → merge → (identical ⇒ skip without CAS) → token-CAS; retried on token
/// conflict only until `deadline`; every attempt re-runs checkFenceOrThrow(admitted_generation)
/// AFTER its read and BEFORE its CAS (the §3 recheck discipline — same value at every site).
CkptPublishOutcome publishCkpt(Backend &, const Layout &, const RootNamespace &,
                               const RefCkpt & contribution, uint64_t admitted_generation,
                               const std::function<void(uint64_t)> & check_fence_or_throw,
                               Deadline deadline);
/// check_fence_or_throw is the PLUMBED callback (CasPool wires it from CasMountRuntime the way
/// CasPlainObjects gets fence_generation_fn at CasPool.cpp:171) — the ledger never owns a
/// CasMountRuntime [codex r2 finding 18].
```
- Snapshot-deletability rule this object enables (consumed by Task 9): snapshots are deletable
  only STRICTLY BELOW `checkpoint_snapshot_id` — a stale pointer can only under-clean.
- Missing-sampled-base revalidation (consumed by Task 6): reader samples `_ckpt`, GETs the
  named snapshot, gets 404 → reread `_ckpt`: token advanced → restart; token unchanged →
  `CORRUPTED_DATA` (three-way rule, spec INV-4 verbatim).

- [ ] **Step 1: Failing tests** in `gtest_cas_ref_ckpt.cpp`:
  `mergeCkpt` per-field table (each field independently newer on either side — 6 cases; both
  none; equal bodies); codec round-trip + strict rejections (unknown key, duplicate key,
  truncation → `CORRUPTED_DATA`); `publishCkpt` over InMemoryBackend: create-if-absent when no
  object; token conflict (concurrent writer advanced it) → re-read → merge → second CAS wins;
  identical merged body → `IdenticalSkip` and NO write issued (assert backend op count);
  fence bumped between read and CAS → `FencedOut`, nothing written; deadline exhausted on
  persistent conflict → throws retry-later (fail closed, no partial state); **at the REAL
  snapshot-publisher call site** [codex finding 4]: a committed snapshot publish advances
  `checkpoint_snapshot_id`; the body-PUT/cleanup/`_ckpt` race (cleanup planned between body PUT
  and `_ckpt` CAS cannot delete the just-published snapshot — the deletability rule reads the
  ADVANCED checkpoint); identical-body no-op, token conflict, and stale-generation refusal all
  exercised through `trySnapshotPublishOnce`, not only through the helper directly.
- [ ] **Step 2:** Run `--gtest_filter='CasRefCkpt*'` → FAIL.
- [ ] **Step 3:** Implement format (strict grammar: all three keys, optionals encoded-when-set,
  duplicates rejected), registry entry, layout key, `mergeCkpt` (ONE function — the ledger
  obligation from TLA Task 1: "one shared semantic-max helper + per-field unit test"), and
  `publishCkpt`.
- [ ] **Step 4:** New filter PASS + full CA gate no regressions.
- [ ] **Step 5: Commit** `ca: ref — _ckpt object: strict codec, shared semantic-max merge, fence-rechecked CAS`.

### Task 6: Recovery CAS-walk; install-lock generation recheck; retire the sentinel seal {#task-6}

**Files:**
- Modify: `.../Pool/CasRefLedger.cpp` — `ensureRefTableRecovered` `:394-727` (the whole recovery
  sequence), `installRecoveryResult` `:738`; DELETE the sentinel-seal writer `:601-659`
- Modify: `.../Pool/CasPool.cpp` — the self-remount path (`tryRemountOnce`, `:1018` region):
  the cancel-or-join barrier before fence rearm [codex finding 10]
- Modify: `.../Formats/CasRefSnapshotFormat.h` — retire the sentinel-seal shape (`:20-25` doc,
  `sealed_from` `:57`, the `snapshot_id < *sealed_from` check `:74`): `sealed_from` is REMOVED
  from the format; a decoded snapshot carrying it (old pool) is unreachable behind Task 3's
  format floor, so the field is deleted outright, not tolerated (Constraint 4)
- Create: `src/Disks/tests/gtest_cas_ref_recovery_cas_walk.cpp`

**Interfaces:**
- Consumes: Task 1 grammar (`prev_epoch_seal`, `validateEpochSealGrammarContextual`), Task 2 `slotOccupy`,
  Task 4 wedge adoption, Task 5 `publishCkpt` + three-way revalidation,
  `checkFenceOrThrow` (`CasMountRuntime.cpp:105`).
- Produces: recovery per spec §4 — the sequence Stage B extends with the catalog step.

New `ensureRefTableRecovered` sequence (replaces the LIST-reconciliation core; the outer
transient-retry loop `:441`, restart budget `kRefRecoveryMaxRestarts` `:446-460`, and the
`recovery_in_progress`/`recovery_cv` serialization `:413-428` all STAY):
1. Capture `admitted_generation = mount_runtime.fenceGeneration()` at entry.
2. Read `_ckpt` (may be absent for a namespace born before its first publication this stage —
   then all fields none).
3. ONE hint LIST of the namespace prefix (today's `:472-503` — kept as a HINT: it seeds the
   snapshot candidate and the tail id set; its completeness is NEVER assumed).
4. Exact-key GET of the chosen snapshot (greatest of hint ∪ `_ckpt.checkpoint`); on 404 apply
   Task 5's three-way rule (restart vs `CORRUPTED_DATA`).
5. Arithmetic tail: from `snapshot_id + 1` upward, exact-key GET each id — ids the hint
   mentioned AND the holes it did not (a hint omission is fetched by exact key, found or not);
   first absent id ends the epoch's dense region. A 404 at an id BELOW a durable same-epoch
   higher id (a hint entry that GETs successfully, or `_ckpt.checkpoint`) → vanish-restart
   (`:557` pattern), budget-bounded, then FAIL CLOSED (`CORRUPTED_DATA`) — never "fold what we
   have".
6. Epoch transitions: CAS-walk. For each dead epoch `E` from the recovered position to
   `live_epoch - 1`: `slotOccupy(refLogKey(ns, {E, T_E + 1}), encoded EpochSeal txn, fence_ok)`
   where the seal txn carries `prev_epoch_seal` per grammar (chained across burned/empty epochs —
   `CasPool.cpp:576-611` mints unreclaimed epochs, so consecutive empty seals are NORMAL);
   `Created` → epoch closed by us; `Occupied` + seal bytes → a concurrent recoverer closed it —
   adopt and continue (the `CaCasMountCore` Occupied-seal branch); `Occupied` + non-seal bytes →
   a straggler landed at `T_E + 1` — ADOPT it (apply the txn), `T_E += 1`, retry the seal at the
   NEW `T_E + 1` (never mint `T_E + 2` blindly — state-derived ids, INV-2); `Unresolved` →
   transient-retry path (outer loop), fail closed on budget.
7. `publishCkpt` with `last_epoch_seal = max(walked seals)` under `admitted_generation`.
8. Re-acquire `rt.state_mutex` (`:638-641` pattern), then — NEW, the r9-5 #4 site —
   `mount_runtime.checkFenceOrThrow(admitted_generation)` IMMEDIATELY BEFORE
   `installRecoveryResult` `:665`: a recovery whose I/O window overlapped a fence bump must
   never install (today there is NO such recheck — the site map confirms the gap).

**The three same-value sites are generation-only** [codex finding 7]: the ONE captured
`admitted_generation` from step 1 is what slot-occupy's `fence_ok` (step 6), the `_ckpt` CAS
(step 7) and the install recheck (step 8) all present — one capture point, three checks, no
re-derivation. (Tokens are per-object credentials — the `_ckpt` token in step 7 and, in Stage
B, the catalog-entry token — and are NOT part of this trio.) `installRecoveryResult` also
records the walked chain's last seal id into `RefTableRuntime::last_epoch_seal` (the
blocker-2 hand-off Task 4 consumes).

**Self-remount barrier** [codex finding 10 — spec §3: "self-remount cancels or waits out
recovery before rearming"]: the install-lock recheck alone is not the remount rule. Add the
orchestration in `CasPool.cpp`'s remount path (`tryRemountOnce` area, `:1018` region) + a
cancel-or-join primitive on the recovery state (`recovery_in_progress`/`recovery_cv` `:413-428`
extended with a `cancel_requested` flag the recovery loop polls at each I/O boundary): fence
REARM may not complete while a recovery attempt is in flight — remount either cancels it (flag
+ cv wait for acknowledgment) or joins its completion; either way no old-`admitted_generation`
`_ckpt` CAS or install can follow the rearm.

- [ ] **Step 1: Failing tests** in `gtest_cas_ref_recovery_cas_walk.cpp`:
  hint omits a middle id that exists (arithmetic finds it by exact GET — recovery result
  identical to a complete hint; TODAY the reconciliation replay misses it: this is THE blocker's
  recovery face); hint omits the TAIL id (same); 404 below a durable higher id → restart then
  fail closed after budget; dead epoch closed by our seal (`Created`), by a concurrent
  recoverer's seal (`Occupied` seal — adopt), straggler at `T+1` (`Occupied` non-seal — adopt,
  reseal at new `T+1`); two consecutive burned empty epochs → two chained sequence-1 seals with
  correct `prev_epoch_seal`; fence bumped during the walk → install refused (throws), state NOT
  installed, retry under the new generation succeeds; `_ckpt` names a snapshot the hint lost →
  step-4 three-way behavior; **the trio's deterministic bump points** [codex r2 finding 7]:
  fence bumped AFTER slot-occupy succeeded but BEFORE the `_ckpt` CAS → `_ckpt` CAS refuses,
  no install; fence bumped AFTER the `_ckpt` CAS but BEFORE install → install refuses (state
  not published, `_ckpt` advance harmless — semantic-max is idempotent for the retry); plus the
  generic mid-walk bump (any earlier point) → refused at the next site; **remount barrier**
  [codex finding 10]: recovery paused at an I/O fault seam → self-remount initiated → fence
  rearm BLOCKS until the recovery acknowledges cancellation; after release the cancelled
  recovery performs ZERO `_ckpt` CASes and ZERO installs (op-journal assert); **genesis
  recovery** [codex r2 finding 2]: recovery of a namespace born at epoch 5 starts its walk at
  `life_epoch = 5` (no phantom seals for epochs 1-4 are expected or written) and installs
  `last_epoch_seal = none` when no transition ever happened.
- [ ] **Step 2:** Run `--gtest_filter='CasRefRecoveryCasWalk*'` → FAIL.
- [ ] **Step 3:** Implement, DELETING: the sentinel-seal writer block `:601-659`, the
  `sealed_from` field + sentinel doc in `CasRefSnapshotFormat.h`, and the reconciliation's
  trust in `greatest_listed_id` as a completeness bound (`:509-511` — the variable may survive
  as the hint seed, its AUTHORITY does not). Each deletion is Constraint-3 material: the
  replacement is the arithmetic walk, not a quieter fallback.
- [ ] **Step 4:** New filter PASS + full CA gate; expect fallout in recovery-adjacent gtests
  (`RefSnapshotCodec*`, seal-related cases) — fix, report each.
- [ ] **Step 5: Commit** `ca: ref — recovery = ckpt + arithmetic tail + seal CAS-walk; install-lock fence recheck; sentinel seal retired`.

### Task 7: Fold arithmetic intake; seals cross epochs; B1/B2 accounting {#task-7}

**Files:**
- Modify: `.../Gc/CasGc.cpp` — intake loop `:1420-1605` (cursor-advance site `:1590-1596`),
  B1 identity recomputation `:1636-1655`, the B1 enforcement `:2004-2008`
- Create: `src/Disks/tests/gtest_cas_gc_arithmetic_intake.cpp`

**Interfaces:**
- Consumes: Task 1 (`refLogTxnIsEpochSeal`), Task 3 (density invariant), the round's hint
  enumeration (`groupRefKeys` `:1049` — UNCHANGED this task; it becomes purely a hint).
- Produces: per-namespace arithmetic advance — the `CaRefDeltaIntakeCore` `WalkStep` in C++;
  vocabulary for Tasks 8/9: `expected_next(cursor) = cursor.ref_sequence + 1` within the
  cursor's epoch.

Behavior (spec §5): the per-namespace loop no longer iterates `listing.logs` (`:1457-1459`) —
it steps `expected = cursor + 1`, GETs the exact key (the per-record GET was always owed — §8
cost table), applies, advances (`:1590` site). The hint's only roles: (a) namespace membership
in `ref_tables`, (b) a witness set for Task 8's impossible-shape detection. A hint hole at an id
the GET finds — the observed `0x1430c`/`0x1430d` shape — folds through UNNOTICED (no anomaly, no
abort: this is the blocker becoming a non-event). A 404 at `expected` with no witness above =
the namespace's frontier this round (normal end). Epochs are crossed ONLY by consuming a seal at
`expected`: apply as a table no-op, count it applied (B2 `produced=false`), cursor := seal id,
continue at `{next_epoch_with_records_or_seal, 1}` — the seal's `prev_epoch_seal` chain makes
the next epoch's start arithmetic, not guessed. A record of a LATER epoch reachable while the
current epoch lacks its consumed seal is an impossible shape → held (mechanism Task 8; this task
plants the detection point and a plain `classification = 4` clamp as today).

- [ ] **Step 1: Failing tests** in `gtest_cas_gc_arithmetic_intake.cpp` (InMemoryBackend, a
  ledger writer producing real records, fold run against a CONTROLLED hint set):
  hint omits two middle records that exist → fold applies ALL records, cursor = true tail
  (TODAY fails: listed-only iteration skips them and seals past — the blocker reproduced as a
  unit test); hint omits the entire namespace → Task 9 covers (frontier), here: cursor
  untouched, no destruction interplay asserted yet; seal at `T+1` → crossed, `logs_applied`
  counts it, `produced=false` (B2), cursor lands on the seal; two chained empty-epoch seals →
  both consumed in one round; record above an unconsumed seal position visible in hint while
  `expected` GET 404s → classification 4 (clamp), cursor NOT advanced past the gap; B1
  `logs_accounted == logs_applied` holds over a seal-crossing cut (regression for `:2004-2008`).
- [ ] **Step 2:** Run `--gtest_filter='CasGcArithmeticIntake*'` → FAIL.
- [ ] **Step 3:** Implement. The three existing non-gap failure shapes keep their semantics at
  the new sites: body vanished mid-walk → abort `:1474` analog; invalid body → abort `:1495`
  analog; missing manifest body → per-table clamp `:1570` analog (dead-precommit escape
  `:1531-1550` preserved). Whole-round abort (`:1610-1626` discard) NARROWS per spec §5: it
  remains ONLY for a key unattributable to any namespace; every per-namespace failure is a
  per-namespace clamp/hold, never a round abort (add one test: a corrupt body in `ns_a` clamps
  `ns_a` while `ns_b` folds and seals normally).
- [ ] **Step 4:** New filter PASS + full CA gate + the CA-s3 integration lane
  `test_content_addressed_gc_s3` locally (`with_rustfs`) green, log to `<build>/task7_integration.log`.
- [ ] **Step 5: Commit** `ca: gc — arithmetic fold intake; hint demoted; seals cross epochs (B2 no-op)`.

### Task 8: Holds — classification-4 strict grammar, checkpoint witness, REBUILD carry {#task-8}

**Files:**
- Modify: `.../Formats/CasFoldSealFormat.h` / `.cpp` — `ShardCoverage` `:37` gains the hold
  fields; encode `:83` / decode `:189` region; the writer's pre-PUT size gate
- Modify: `.../Gc/CasGc.cpp` — hold creation (Task 7's detection points), hold carry in the
  round CAS, REBUILD carry (`rebuildBaseline` `:2488`), seal-bytes measurement `:2014`
- Create: `src/Disks/tests/gtest_cas_gc_hold_grammar.cpp`

**Interfaces:**
- Consumes: Task 7's detection points; Task 5's `_ckpt` (checkpoint = hint-independent second
  witness); `putDeterministicArtifact` (`Gc/CasBlobInDegree.h:136`) for seal writes (unchanged).
- Produces (Task 9/11 consume):
```cpp
enum class HoldReason : uint8_t   /// bounded ENUM — strict grammar, spec r9-4
{
    GapBelowWitness = 1,        /// 404 at expected, durable same-epoch witness above
    UnconsumedSealCrossing = 2, /// later-epoch record reachable, current epoch's seal not consumed
    WitnessDisappeared = 3,     /// an above-cursor witness stopped GETting — corruption, never clearance
    BodyUndecodable = 4,
};
/// ShardCoverage extension — fields REQUIRED iff classification == 4, FORBIDDEN otherwise:
///   HoldReason hold_reason; RefTxnId offending_position; uint32_t retry_count; uint64_t next_retry_round;
```
- The clearing rule: a hold clears ONLY by folding through `offending_position` and adopting the
  result in `gc/state` — NEVER by observing another absent (spec §5; the
  `CaRefDeltaIntakeCore` hold/holdDebt semantics).
- A witness that disappears while the gap remains → `WitnessDisappeared`, still held (an
  above-cursor witness cannot be legitimately cleaned — its disappearance is corruption).

- [ ] **Step 1: Failing codec tests** in `gtest_cas_gc_hold_grammar.cpp`: round-trip all four
  reasons; classification 2 carrying a hold field → `CORRUPTED_DATA`; classification 4 missing
  any hold field → `CORRUPTED_DATA`; duplicate hold keys → `CORRUPTED_DATA`; boundary AND
  boundary-plus-one byte-reservation tests — with the TWO caps kept distinct [codex finding 9]:
  (a) PER-ROW framing vs the 64 KiB LINE cap (`Formats/CasFormat.cpp:102` registry): the
  maximal hold row (max enum, max numerics, worst-case escaping) encodes within a line; one
  byte of payload growth over → strict decode rejects; (b) WHOLE-SEAL size vs the 256 MiB
  OBJECT cap: a seal built to land EXACTLY at the object cap is ACCEPTED (equality passes);
  cap + 1 → the writer's NEW pre-PUT gate throws retry-later BEFORE the PUT (today
  `CasGc.cpp:2014` measures only). One shared byte-arithmetic helper computes both predicates
  (`encoded row <= line_cap`; `fold_fixed_bytes + Σ worst_case_entry_reservation <= object_cap`)
  — Stage B Task 2 reuses THIS helper for the catalog's additive predicate. The pre-PUT gate
  guards BOTH seal write sites: the regular fold write (`CasGc.cpp:2018` region) and REBUILD's
  second `encodeFoldSeal` PUT (`:2861` region).
- [ ] **Step 2:** `--gtest_filter='CasGcHoldGrammar*'` → FAIL.
- [ ] **Step 3: Behavior tests** (same file): gap-below-witness (hint witness) → hold with exact
  `offending_position`; gap below `_ckpt.checkpoint` with hint SILENT about the namespace's tail
  → SAME hold (checkpoint is the second witness — `_fix_ckptwitness`'s C++ twin); hold carried
  verbatim across a round where the hint omits the namespace entirely; hold forces an exact
  retry of `offending_position` even when unhinted; clears only by folding through (plant the
  record → next round folds → classification back to 2); witness disappears, gap remains →
  `WitnessDisappeared`, held; REBUILD (`rebuildBaseline`) carries every hold VERBATIM into the
  rebuilt baseline; REBUILD with a missing prior fold seal → REFUSES (throws, names pool
  recreation — spec r9-1); undecodable prior seal → same refusal.
- [ ] **Step 4:** Implement; new filter PASS + full CA gate.
- [ ] **Step 5: Commit** `ca: gc — durable holds: strict classification-4 grammar, ckpt witness, REBUILD carry + refusal`.

### Task 9: Frontier proof — `suppress_destructive` before EVERY destructive site {#task-9}

**Files:**
- Modify: `.../Gc/CasGc.cpp` — the destructive-gate computation (`:1833` site), the three
  UN-gated sites from the audit map: manifest-body deletes `:841-864`, generation prune
  `:2316`/`deletePrefixWholesale` `:2282`, orphan-sweep invocation `:912`; cleanup ranges in
  `cleanupRefObjects` `:2075`
- Modify: `.../Gc/CasBlobInDegree.cpp` — `settleEntry` `:414-469` (normative comment + metric)
- Create: `src/Disks/tests/gtest_cas_gc_frontier_gate.cpp`

**Interfaces:**
- Consumes: Task 7 (walk-to-absent = frontier proof for hinted-active namespaces), Task 8
  (holds), `FoldResult::suppress_destructive` (`Gc/CasGc.h:352`).
- Produces: the `DestructiveGate` (model vocabulary): a round performs destructive work ONLY
  holding a frontier proof for EVERY known namespace — Stage A's universe = (namespaces in
  `gc/state` cursors) ∪ (this round's hint); the Stage-B catalog swaps the universe source
  under the SAME gate.

Behavior (spec §5): quiet namespaces (known, unhinted or hinted-inactive) each get ONE exact
`GET expected_next(cursor)`: absent → frontier proven; present → the namespace was WRONGLY
quiet → walk it this round (Task 7 loop). Budget exhausted before every proof lands → cursor
advances may still seal, ALL destruction suppressed this round. `suppress_destructive` becomes
`anomalies present OR any hold carried OR frontier incomplete`, and is CONSULTED at every
destructive site. **The Stage-A universe seam:** `frontier_incomplete` is computed as
`policy != UniversePolicy::AuthoritativeForTest || <per-namespace proofs missing>` where the
destructive-gate computation receives `UniversePolicy policy = UniversePolicy::kDefault` and
`kDefault = StageA_Suppressed` in Stage A (staging contract; the comment at the enum cites
codex finding 1's cross-namespace scenario verbatim); gtests reach the per-namespace logic by
passing `AuthoritativeForTest` explicitly at the fold-entry call with CLOSED universes — and
the seam is PRODUCTION-UNREACHABLE [codex r2 finding 1]: `GcTestHooks` is declared in a
test-only header compiled solely into `unit_tests_dbms` (no production translation unit
includes it), and the injection is a PARAMETER, not an override [codex r3 finding 1 — a test-only header cannot
override a constant compiled into production code]: the enum's only other value
`AuthoritativeForTest` is passed EXPLICITLY by gtests at the fold-entry call; the single
production call site passes nothing. No config, no env, no runtime flag reads the policy —
Stage B's Task 7b changes `kDefault` itself (a source change), which is the entire flip. There
is NO other seam: no `GcTestHooks`, no mutable global.
**Destructive-site inventory is a STEP, not an assumption** [codex finding 6]: before wiring,
run a tree-wide inventory of every `deleteExact`/`deletePrefixWholesale`/delete-marker call
reachable from GC and list each with its gate status in the task report — the known un-gated
set is manifest-body deletes `:841-864`, generation prune `:2316`/`:2282`, orphan-sweep
invocation `:912`, AND the post-CAS `handoff_reclaim` wholesale generation-prefix delete
`:793-833` (which today runs BEFORE `suppress_destructive` is even assigned at `:879` — the
gate must be computed and transported ahead of it); the inventory may find more — every found
site gets the gate and its own zero-delete-under-suppression test.
The delete-site in-degree re-read (`settleEntry` `:414-469` sparing `indeg > 0`, then
`deleteExact` `:516`) is hereby NORMATIVE (spec §5 third arm) — it gets the comment saying so
and a regression test; to prove the guard ITSELF can fail, the test uses a test-only seam that
bypasses the final in-degree read, records the targeted test RED, restores the guard, records
GREEN [codex finding 16].

- [ ] **Step 1: Failing tests** in `gtest_cas_gc_frontier_gate.cpp`:
  a hidden `+1` in a KNOWN-but-unhinted namespace while a `-1` elsewhere is ready to graduate →
  destruction suppressed (the cross-namespace hidden-`+1` control — the load-bearing RED of the
  whole phase, now C++; runs under the test seam with a closed universe); same round, the
  quiet-probe GET finds the `+1` → namespace walked, fold adopts it, destruction proceeds NEXT
  round; frontier budget exhausted → seals happen, zero deletes (assert backend delete-op
  count == 0); every inventoried destructive site individually (at minimum: manifest-body
  delete, generation prune, orphan sweep, `handoff_reclaim`) — all inert under suppression
  (assert per-site, delete-op count == 0 each) [codex finding 6]; production default
  (`UniversePolicy::kDefault == StageA_Suppressed`, no explicit policy argument) → destruction
  inert even with every constructed proof green; late `+1` folded AFTER condemnation but BEFORE the delete
  pass → `settleEntry` spares it (`indeg > 0`) — the normative third-arm regression (with the
  seam-RED proof, above); **the temporal lemma's OTHER arms, pinned in C++** [codex finding 5]:
  (a) a post-probe `+1` followed by SAME-round condemnation → the newly condemned blob is not
  deleted in that round (the not-same-round rule directly asserted); (b) SOURCE-BACKED/TOKENED
  adoption of an already-delete-pending blob → reads `Condemned` meta, rematerializes from
  source as a fresh incarnation, and the delayed old-token `deleteExact` cannot remove the new
  object (token mismatch asserted); (c) TOKENLESS relink (`adoptEvidence`) → an operation
  journal proves the receiver's `+1` is durable BEFORE the source releases its committed edge,
  and no interleaving in the test's schedule produces a window where the blob has zero durable
  owners; covered-log cleanup deletes exactly the computable range under
  `min(_ckpt.checkpoint, cursor)` and nothing above (ranges, not listings); snapshot cleanup
  deletes only STRICTLY BELOW `_ckpt.checkpoint` — a snapshot AT the checkpoint survives
  (Task 5's rule, asserted at the delete site).
- [ ] **Step 2:** `--gtest_filter='CasGcFrontierGate*'` → FAIL.
- [ ] **Step 3:** Implement. `discoverUniverse` (`:2393`) is UNCHANGED in mechanism (still the
  hint) but its result now only ADDS to the known-namespace set (union with `gc/state` cursors);
  it can no longer shrink the frontier obligation.
- [ ] **Step 4:** New filter PASS + full CA gate + `test_content_addressed_gc_s3` local lane green.
- [ ] **Step 5: Commit** `ca: gc — destructive-round frontier proof; suppress_destructive at every destructive site`.

### Task 10: Sweep §6 deletion premise {#task-10}

**Files:**
- Modify: `.../Gc/CasOrphanManifestSweep.cpp` — `sweepNamespace` `:272` (delete decision around
  `:316-327`), `sweepManifestCursorPage` `:342` (delete decision around `:434`),
  `prefixEligible` `:252`
- Create: `src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp`

**Interfaces:**
- Consumes: Task 7 (cursor semantics: "epoch `E`'s seal consumed" ⇔ cursor has crossed a seal
  with `writer_epoch >= E`), Task 9 (`suppress_destructive` — the sweep is already behind the
  frontier gate after Task 9; this task adds the PER-MANIFEST premise).
- Produces: the two §6 rules, verbatim from the spec, as ONE predicate both sweep paths call:
```cpp
/// A manifest of an epoch-E build is deletable only when:
///   (1) the namespace cursor has consumed epoch E's seal, AND
///   (2) no unconsumed tail record above the cursor names it as a removal target
///       (removals cross epochs; grants do not).
/// ANY uncertainty — unreached frontier, budget exhaustion, hold — means retain.
bool manifestDeletionPremise(const NamespaceFoldView &, const ManifestKey &, String * retain_reason);
```

This task deliberately does NOT implement register R3 (nomination path) or R2 (writer duty
queue) — those are Stage B scope as one coherent sweep change; the premise here is the SAFETY
floor that must hold regardless. `retain_reason` feeds the existing `warnings` out-param
(`sweepNamespace`'s `std::vector<String> * warnings`) — retained-manifest decisions are visible,
not silent (Constraint 10).

- [ ] **Step 1: Failing tests** in `gtest_cas_sweep_deletion_premise.cpp`:
  manifest of epoch `E`, seal of `E` NOT yet consumed → retained with reason; seal consumed, an
  unconsumed tail removal above the cursor names the manifest → retained; seal consumed, tail
  clean → deleted (exact token — the existing `deleteExact` path `:327`/`:434`); hold present
  on the namespace → retained (uncertainty rule); budget exhausted mid-page → remaining
  candidates retained, cursor does NOT skip them.
- [ ] **Step 2:** `--gtest_filter='CasSweepDeletionPremise*'` → FAIL.
- [ ] **Step 3:** Implement the predicate + wire both call sites; `prefixEligible` (`:252`)
  keeps its watermark logic and ADDITIONALLY requires the premise (belt over suspenders is fine
  here — the watermark is an eligibility hint, the premise is the safety floor).
- [ ] **Step 4:** New filter PASS + full CA gate.
- [ ] **Step 5: Commit** `ca: sweep — §6 deletion premise: seal-consumed + no-tail-removal, retain on uncertainty`.

### Task 11: REBUILD condemn-nothing; fsck arithmetic streams {#task-11}

**Files:**
- Modify: `.../Gc/CasGc.cpp` — `rebuildBaseline` `:2488`: DELETE the condemnation block
  `:2739-2800` (the blobs-prefix LIST `:2756`, `edge_bearing` skip `:2762`, HEAD `:2764`,
  `zero_condemned` insertion `:2767-2774`, synchronous condemn markers `:2783-2800`)
- Modify: the fsck implementation (locate via `grep -rn "chain-broken\|unchecked" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` — the fsck entry point and its summary/exit-code plumbing; the Task-0 tree makes this unambiguous)
- Create: `src/Disks/tests/gtest_cas_rebuild_condemn_nothing.cpp`

**Interfaces:**
- Consumes: Task 8 (hold carry through REBUILD + refusal — already landed there), Task 7
  (arithmetic streams).
- Produces: REBUILD per spec §7 — rebuilds cursors and edges from `_ckpt` + arithmetic tails,
  **condemns nothing**; fsck per §7 — streams by arithmetic, `chain-broken` fatal in summary
  AND exit code, tails above `_ckpt.checkpoint`, `unchecked` reserved for the genuinely
  unproven, healthy pool returns clean.

The removed condemnation is the r5-finding-4 data-loss vector (hidden live manifest + listed
blob ⇒ acked data condemned). Its ABSENCE creates the NAMED Stage-A residual (staging contract):
manifest-less orphan blobs are unreclaimable until register R4's build/upload registry — the
task's report and the stage summary both restate this residual explicitly; no quiet substitute
reclamation is added (Constraint 3: no fallback).

- [ ] **Step 1: Failing tests** in `gtest_cas_rebuild_condemn_nothing.cpp`:
  a durable blob whose manifest the hint HIDES from REBUILD's traversal → after REBUILD the blob
  is NOT condemned (today FAILS — `:2767` condemns it: the data-loss vector, now a regression
  test); REBUILD output carries pre-existing holds verbatim (Task 8's test extended to the
  full-rebuild path); fsck on a healthy arithmetic pool → clean, exit 0; fsck with a planted
  mid-chain 404 below a witness → `chain-broken` in summary AND nonzero exit; fsck tail above
  `_ckpt.checkpoint` → walked, reported, not `unchecked`; fsck `unchecked` appears ONLY for the
  genuinely unproven class (a namespace with a hold), never as a default.
- [ ] **Step 2:** `--gtest_filter='CasRebuildCondemnNothing*'` → FAIL.
- [ ] **Step 3:** Implement (delete + fsck rework).
- [ ] **Step 4:** New filter PASS + full CA gate + `test_content_addressed_gc_s3` lane green.
- [ ] **Step 5: Commit** `ca: gc — REBUILD condemns nothing; fsck walks arithmetic streams (chain-broken fatal)`.

### Task 12: Retirement sweep — retire-or-justify with verdicts {#task-12}

**Files:**
- Modify (per verdict): `.../Gc/CasGc.cpp` probe A `:1062-1205`; `.../Pool/CasPool.cpp`
  `materialization_grace` `:1018-1029` (+ config plumbing `CasPool.h:152`,
  `ContentAddressedSettings.cpp:91`, `ContentAddressedMetadataStorage.cpp:287,740`, open-path
  `:679`/`:703`); `.../Pool/CasRefLedger.cpp` residual reconciliation leftovers
- Create: `src/Disks/tests/gtest_cas_retirement_sweep.cpp`
- Create: `docs/superpowers/cas/2026-07-28-stage-a-retirement-verdicts.md` (the verdict table —
  one row per item: premise / verdict (a|b|c) / replacement / test)

**Interfaces:**
- Consumes: everything landed in Tasks 3-9 (the premises' killers).
- Produces: the verdict document + the code changes it mandates. THE RULE (user directive,
  ledger `MAIN-PLAN OBLIGATION`): (a) premise dead → REMOVE + fail-close assert in its place;
  (b) premise transformed → re-derive; (c) premise alive → keep with documented reason.
  **No (c) by inertia** — every row cites the Stage-A mechanism that was checked against it.

Item-by-item, with the expected verdict to VERIFY (not assume — the implementer's analysis may
overturn any of these with evidence, which then goes to the controller as a question):
1. **Probe A** (`:1062-1205`) — expected (b): its hole-detection premise (two listings disagree)
   is subsumed by arithmetic intake; its ABORT effect (`ref_folding_aborted` `:1193-1195`)
   contradicts the new design (a hint hole is a non-event; a real gap is a HOLD, not an abort).
   Re-derive as spec §5 mandates: a SAMPLED store-quality detector, deterministic cadence,
   durable due/performed/skipped observability, **aborts nothing, gates nothing** (register R7's
   demotion). The pre-fold second enumeration (`preFoldRefScan`) retires with it → the round has
   ONE strict hint enumeration (spec §5's stated shape).
2. **`materialization_grace` / T_mat wait** (`:1018-1029`, open path `:679`/`:703`) — expected
   (a) for the ref layer: its premise ("an unresolved ref-log conditional PUT from the dying
   epoch lands after recovery trusts its LISTINGS") dies — recovery no longer trusts listings
   for completeness, and the in-band seal at `T+1` fences stragglers deterministically
   (Task 6). Verify the wait guards NOTHING ELSE (the site map ties it to
   `refLanesSettledForRemount` + `unclean_boundary_epoch`, both ref-layer); if the analysis
   confirms, DELETE the wait + `unclean_boundary_epoch` coupling (`CasRefLedger.cpp:611` gate
   died with Task 6's sentinel-seal removal; `:921` doc updated) AND delete the setting
   outright — no parsed-but-inert period, no deprecation log: the feature never shipped, there
   are no configs to protect (user directive 2026-07-28; recreate-only, Constraint 4).
3. **Recovery LIST-reconciliation trust** — verdict (a) ALREADY EXECUTED in Task 6 (listed here
   so the table is complete; row cites Task 6's commit).
4. **Fold listed-only iteration** — (a) executed in Task 7 (row cites commit).
5. **`greatest_listed_id` as authority** — (a) executed in Task 6.
6. **404-never-throw during GC fold** — (c) KEEP: its reason (concurrent legitimate deletion,
   record-and-continue) is about CONCURRENT DELETES, not LIST trust — unchanged by v9. Row
   documents this.
7. **Re-hash identity / HEAD-before-PUT / condemned-never-revived** — (c) KEEP: adversary-model
   primitives, explicitly out of scope of any "optimization" (Constraint 5). Row documents.

- [ ] **Step 1:** Write the verdict document with the table above as its skeleton; for rows
  1-2 perform the code-level analysis FIRST (read every consumer; the config plumbing list
  above is the map's complete set) and record evidence per row.
- [ ] **Step 2: Failing tests** in `gtest_cas_retirement_sweep.cpp`:
  probe A no longer aborts folding on a planted hint hole (fold completes, hole folds through);
  probe A's detector still REPORTS the hole (observability retained: due/performed/skipped
  counters present and durable); remount with an unresolved dying-epoch lane completes WITHOUT
  the T_mat sleep and the straggler is fenced by the seal (fault-injected straggler PUT after
  recovery → conditional create loses to the occupied slot — assert the straggler's PUT
  conflicts); the deleted wait's site has the fail-close replacement (whatever the analysis
  mandates — e.g. `chassert(ref_lanes_settled || wedged)` shape).
- [ ] **Step 3:** Implement per verdicts; run new filter + full CA gate + BOTH local CA-s3
  lanes (`test_content_addressed_s3`, `test_content_addressed_gc_s3`).
- [ ] **Step 4:** Self-check the verdict doc against the ledger rule (no row without a cited
  mechanism; no (c) without a reason that survives v9).
- [ ] **Step 5: Commit** `ca: retirement — probe A demoted to detector; T_mat wait retired; verdict table`.

### Task 13: LIST-liar fault injection — the blocker's end-to-end regression {#task-13}

**Files:**
- Modify: `.../Backend/CasInMemoryBackend.{h,cpp}` (`list` — add the liar hook) and, if the
  emulated single-process backend's `list` is a distinct path
  (`CasObjectStorageBackend.cpp` emu arm), the same hook there
- Create: `src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp`

**Interfaces:**
- Consumes: everything (this is the integration regression of the whole stage).
- Produces: `void setListOmissions(std::vector<String> hidden_keys)` on the test backends —
  enumeration omits the named keys while `get`/`head`/`putIfAbsent`/`casPut`/`deleteExact`
  serve them honestly (EXACTLY the RustFS defect shape from
  `reports/2026-07-26-list-incompleteness-proof/`: durable objects invisible to LIST while a
  LATER key is visible).

- [ ] **Step 1: Failing tests** (they fail before Stage A only if run against pre-Stage-A code —
  on the completed branch they must PASS; the "failing first" step here is running each against
  a deliberately broken toggle, see Step 3):
  **(the blocker, full pipeline)** writer appends ids 1..5; LIST hides 3 and 4 (returns 1,2,5);
  GC folds → all five folded, cursor at 5, zero anomalies (the `0x1430c`/`0x1430d` shape as a
  non-event); recovery under the same lie → recovered state identical to truth; **(the
  data-loss arm)** a `-1` for a blob rides id 3 (hidden) while its blob's last other `+1` is
  visible → blob NOT deleted this round (in-degree correct after arithmetic fold); **(the
  leak arm)** a `+1` rides the hidden id → blob's in-degree includes it, later legitimate `-1`
  does not strand it; **(the cross-namespace kill shot — the staging suppression's regression)**
  namespace `A` absent from BOTH hint and `gc/state`, its acked `+1` on a shared blob hidden,
  while namespace `B`'s visible `-1` drives the blob's observable in-degree to zero → REQUIRED
  VERDICT: ZERO deletes (the `StageA_Suppressed` default holds destruction); the SAME test with
  an explicit `AuthoritativeForTest` argument and `A` absent from the constructed universe MUST delete
  the blob — proving the suppression is the only thing between Stage A and this data loss, i.e.
  the constant is load-bearing and Stage B's catalog is its honest replacement [codex finding 1];
  **(fsck)** fsck under the lie → clean (arithmetic streams don't consult LIST
  completeness).
- [ ] **Step 2:** Wire the hook (test-only; production `list` untouched).
- [ ] **Step 3:** Prove each test CAN fail: temporarily revert the Task 7 intake commit in a
  scratch worktree (`git worktree add` — do NOT touch the main tree) and run the suite there —
  the blocker test must FAIL under listed-only intake. Record the failure output in the report
  (this is the plan's "seen red" discipline applied to C++).
- [ ] **Step 4:** Full CA gate + both CA-s3 lanes green.
- [ ] **Step 5: Commit** `ca: tests — LIST-liar fault injection; the 2026-07-25 blocker as a permanent regression`.

### Task 14: Stage A gates — full battery + soak + stage summary {#task-14}

**Files:**
- Create: `docs/superpowers/cas/2026-07-28-stage-a-RESULTS.md`

**Interfaces:**
- Consumes: all prior tasks.
- Produces: the Stage-A gate verdict line — `STAGE A: PASS` / `FAIL` — the precondition of
  Stage B's Task 0 AND of any user-facing "the blocker is closed" claim.

- [ ] **Step 1:** Full CA gtest gate (Task 0's filter) — record counts vs Task 0 baseline;
  every intentional test change already itemized in task reports.
- [ ] **Step 2:** Integration: ALL CA lanes locally (`with_rustfs` set:
  `test_content_addressed_s3`, `test_content_addressed_gc_s3`, `test_content_addressed_shared_pool`,
  `test_content_addressed_drop_pool_member`, `test_content_addressed_ref_snaplog`,
  `test_cas_replicated_relink`, `test_cas_lazy_load_recovery`, `test_cas_insert_fault_recovery`,
  `test_cas_file_cache`) via `python -m ci.praktika run "integration" --test <selectors>`;
  one log per lane in the build dir; a subagent summarizes each log.
- [ ] **Step 3:** Soak: `utils/ca-soak` phase 3 `--duration 90m` (the REAL soak — phase 1
  `--ops` finishes ~10× faster and is NOT a substitute), plus the adversarial scenarios lane
  relevant to this stage (at minimum: the GC concurrent-leader scenarios and S30's
  dangling-precommit shape from `utils/ca-soak/scenarios/`). PASS criteria: zero data-loss
  events, no wedge that SURVIVES a foreground-flush or remount resolution opportunity (a
  permanently quiet unacknowledged wedge with no such opportunity is ALLOWED and reported, per
  register R6 — and the soak asserts NO background deadline-resetting retry loop exists) [codex
  finding 15], fsck clean at end, `unaccounted` growth bounded by the suppressed-destruction
  debris model (destruction is OFF in Stage A — the criterion is "every unaccounted object is
  attributable to a suppressed delete family", not "zero growth"), no
  ERROR-severity log lines that are not test-injected.
- [ ] **Step 4:** Write `2026-07-28-stage-a-RESULTS.md`: battery table (every gate, expected vs
  observed), the residual restatement (staging contract verbatim), and the verdict line.
  `STAGE A: PASS` requires every row green; anything else is `FAIL` with the failing row named
  (no partial credit — the `no known reds` rule).
- [ ] **Step 5: Commit** `ca: stage A — gate battery results + verdict`.

---

## Self-review checklist (run at plan-completion, before removing the DRAFT marker) {#self-review}

1. Spec coverage: INV-1 → T3/T4; INV-2 → T1/T2/T4/T6/T7; INV-4 → T5 (+T8 witness, T9 ranges);
   §4 → T6; §5 → T7/T8/T9; §6 → T10; §7 → T11; §8 cost ceilings → constraint 8; §9 C++ side →
   T13 + per-task seen-red steps. INV-3/§3 catalog rows → Stage B plan (staging contract).
2. Placeholder scan; type consistency across task Interfaces blocks.
3. Ledger obligations mapped: ckpt shared merge helper → T5; three-site same-value recheck →
   T5/T6 (Stage-A sites; catalog site joins in B); install-lock recheck → T6; retirement rule →
   T12; deleteExact normative → T9; capacity byte-half boundary tests → T8 (seal); LIST-liar →
   T13.





