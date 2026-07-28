# CAS merge-layout preparation — design

**Status:** DRAFT (2026-07-28). Spec only; no code landed. **Branch:** `cas-gc-rebuild`.
**Scope:** the *preparatory* work that makes the CAS branch deliverable as a series of
review-sized pull requests. It covers decomposition of the complex hot spots, re-seaming of the
test suite, and the mechanics of producing the series. It does **not** cover landing the series
itself; that follows from this work and gets its own plan.
**Companion:** `docs/superpowers/cas/11-walkthrough.md` (the reviewer-facing description of what
the subsystem does — the audience this preparation serves).

---

## 1. Goal and non-goals

**Goal.** Bring the branch to a state where every genuinely complex algorithm lives in its own
unit, with its own tests, and can be introduced by its own pull request that a reviewer can hold
in their head at one sitting.

**Non-goals.**

- Changing any CAS behaviour. Every step in workstream B is behaviour-preserving by construction.
- Rewriting the existing 3060-commit history. It is not salvageable and will not be used
  (§8).
- Landing the feature. This spec ends when the tree is carveable; the series itself is a plan.
- Closing the two known correctness gaps (cold-`LIST` trust, relink dangle-freedom). They are
  tracked separately and are explicitly *not* prerequisites for merge preparation.

**Targets, decided.** Upstream-affecting fixes go to both upstream ClickHouse and
`altinity/antalya-26.6`, as the first set of small PRs. The CAS feature itself goes to
`altinity/antalya-26.6` only.

---

## 2. Where we are — measured, not estimated

Against `altinity/antalya-26.6` (merge base `4359a07f088`): 3060 commits, 1215 files,
+486 529 / −163.

| Area | Files | Lines |
|---|---:|---:|
| `docs/superpowers/` (worklogs, models, reports) | 540 | +344 377 |
| `src/Disks/tests/` — CAS gtests | 105 | +53 315 |
| `src/.../ContentAddressed/` — the subsystem | 120 | +44 000 |
| `utils/` (ca-soak, scenarios) | 181 | +30 997 |
| `tests/` (stateless + integration) | 146 | +5 971 |
| **`src/` — genuine upstream contact** | **97** | **+4 937 / −129** |
| `programs/`, `.github`, `ci` | 20 | +2 439 |

Two structural facts make the whole plan possible:

1. **The subsystem is a clean layered library.** Dependencies form a DAG —
   `Primitives → Formats → Backend → Pool → {Parts, Gc} → Tools` — with exactly one cycle (a
   single `#include` of `Formats/CasFormat.h` in `Primitives/CasTypes.h`), and **no layer includes
   the top-level glue**.
2. **The feature is reachable through exactly one line**:
   `factory.registerMetadataStorageType("content_addressed", …)` at
   `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:219`.

Together these mean the subsystem can land *dark*: layer by layer, each compiling and unit-tested,
with zero behaviour change, until one small wiring PR turns it on.

### 2.1 Where the complexity actually is

Complexity is concentrated in ten functions, not spread over 120 files.

| File | Worst function | Lines | Share of file |
|---|---|---:|---|
| `Gc/CasGc.cpp` (3134) | `fold` | **1044** | 3 functions = 67% |
| | `runRegularRound` | 642 | |
| | `rebuildBaseline` | 417 | |
| `Tools/CasFsck.cpp` (847) | `runFsckImpl` | **507** | 60% |
| `Pool/CasRefLedger.cpp` (3047) | `flushRefBatch` | 570 | 3 functions = 44% |
| | `commitRefChunk` | 413 | |
| | `ensureRefTableRecovered` | 344 | |
| `Pool/CasPartWriteTxn.cpp` (1490) | `uploadFromSource` | 354 | 2 functions = 40% |
| | `promote` | 250 | |
| `Gc/CasBlobInDegree.cpp` (665) | `foldDeltasIntoGeneration` | 282 | 42% |

Three further files mix responsibilities rather than hosting one giant function:

- **`Pool/CasPool.cpp`** (1636) — **66 methods on a single `Pool` class**: mount lifecycle,
  remount, forget, ref-ledger delegation, staging, plain objects, blob upload, meta, event
  dispatch. The god object of the subsystem.
- **`ContentAddressedTransaction.cpp`** (1916) — hosts **four classes**: the transaction (18
  methods), two write-buffer implementations (`CaContentWriteBuffer`, `CaInlineWriteBuffer`),
  `PartStaging` — plus two stray `ContentAddressedMetadataStorage` methods that belong next door.
- **`ContentAddressedMetadataStorage.cpp`** (2223) — one class in five roles: path routing,
  directory classification, the read/exists/list surface, startup and lifecycle, and the
  `IContentAddressedExchange` relink implementation.

### 2.2 What is deliberately left alone

`Pool/CasServerRoot.cpp` (max function 159, cohesive owner/epoch/mount roles),
`Tools/CasInspect.cpp` (max 70, a renderer of small functions),
`Gc/CasOrphanManifestSweep.cpp` (max 120), `Parts/PartFolderAccess.cpp` (three classes, but a
cohesive facade trio) and `Backend/CasObjectStorageBackend.cpp` (a thick adapter, which is the
expected shape for an adapter).

**The large headers must not be split.** They are 53–61% normative comments over 200–284 lines of
declarations:

| Header | Lines | Comments | Declarations |
|---|---:|---:|---:|
| `Pool/CasPool.h` | 955 | 61% | 284 |
| `Pool/CasRefProtocol.h` | 718 | 61% | 214 |
| `ContentAddressedMetadataStorage.h` | 758 | 55% | 267 |
| `Gc/CasGc.h` | 621 | 53% | 221 |

These headers *are* the specification of the protocols, and they are the single most valuable
asset for a reviewer. When a unit is extracted, its normative comment block moves with it,
unabridged.

---

## 3. Principles

**P1 — Split decision from I/O and locking.** The half that decides becomes a pure function over
explicit state; the half that performs I/O and holds locks becomes a thin shell. This is what makes
an algorithm independently reviewable *and* independently testable, and it is already validated in
this codebase: `Pool/CasRefProtocol.cpp` (862 lines) is exactly this shape — state machine, replay,
admission budgets, edge extraction, and `recoverRefTableDetailed` as a free function reused by GC
and fsck.

**P2 — A pure unit can land with no consumer.** Introducing an algorithm plus exhaustive tests,
with nothing calling it yet, is a complete and reviewable pull request. This is the mechanism that
lets complex algorithms arrive one at a time.

**P3 — Behaviour preservation is verified, not asserted.** Every extraction is gated by the full
gtest suite *and* a soak run (§9). Extraction of concurrent code that silently changes lock
discipline is the primary risk of this entire effort.

**P4 — A test lands with the topmost unit it exercises.** See §6.

**P5 — Normative comments travel with the code.** No extraction may drop or summarise an invariant
comment block.

**P6 — Prefer deletion to relocation.** Where the decomposition surfaces genuinely dead or
duplicated code, remove it rather than carry it into a PR. (Expected to be small; the tree is young.)

---

## 4. Workstream A — upstream carve-out

The first PRs, targeting upstream and `antalya-26.6` in parallel. Independent of everything else
in this spec, so it starts immediately. Source of the inventory:
`docs/superpowers/cas/upstream.md` §G plus the disk-transaction contract argument written there.

**A1 — standalone fixes** (small, each with its own test, easy to justify):
`ReadBufferFromFileView` position corruption (B115), `ReadBufferFromS3` retry-cancel (B117),
`ThreadStatus` lifetime (B90), `LocalObjectStorage` TOCTOU, `MergeTreeDeduplicationLog` null-writer,
`copyS3File` `message_format_string`, `Expect: 100-continue`.

**A2 — object-storage conditional-write API**: `S3Exception::isPreconditionFailed`, token/conditional
surface on `IObjectStorage`, conditional PUT/COPY in `S3ObjectStorage` and `copyS3File`, the GCS
dialect and GOOG4 signer with their tests.

**A3 — disk-transaction contract** (the opinionated one): projection sub-parts routed through the
parent whole-part transaction, read-your-writes API on `IDiskTransaction`, whole-part transaction in
clone/freeze, explicit ordering of staged operations.

A1 and A2 are straightforward. **A3 will need discussion upstream and must not block CAS**: it
lands in `antalya-26.6` on our schedule and proceeds upstream at its own pace.

**A4 — `DataPartsExchange`.** `src/Storages/MergeTree/DataPartsExchange.cpp` takes +650 lines,
the largest single change to an existing shared file in the branch: `relinkPartToDisk`, the
two-trip publish-confirm handshake, and the seven-row failure taxonomy. It is CAS-specific, so it
goes only to `antalya-26.6`, but it gets **its own PR and its own reviewer** — its review cost is
disproportionate to its size.

---

## 5. Workstream B — hotspot decomposition

Behaviour-preserving. Executed on the working branch, before any carving (§8 explains why).
Ordered by complexity-per-line, highest first.

### B1 — `Gc/CasGc.cpp`

The largest and most conceptually loaded file in the subsystem. Target units:

- **Round driver** — the 18-phase sequence. The phase names already exist as string literals and
  as the enum in `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:63`; each becomes a
  named function. The driver becomes a readable list of phase calls.
- **Fold** — ref-list grouping, seal reads, per-log intake with atomic staging, the clamp/barrier
  rules, edge emission.
- **Merge / settlement** — the three-cursor merge. Partly pure already (`settleEntry`); finish the
  job so the spare/graduate/redelete/carry decision is a pure function of
  `(entry, in-degree, round, suppress_destructive)`.
- **Lease** — acquire/renew/steal plus the heartbeat, including the two-signal liveness evaluation.
- **Rebuild** — `rebuildBaseline`, the disaster-recovery path.
- **Prune** — generation retention, hand-off reclaim, ref-object cleanup, namespace cleanup.

Pure predicates to extract explicitly, because each is a documented decision rule that deserves its
own tests: the defer decision, the clamp/suppress-destructive decision, the settlement rules, the
ref-cleanup plan (`planRefCleanup` is already pure — keep it as the model), and the graduation gate.

### B2 — `Tools/CasFsck.cpp`

`runFsckImpl` (507 of 847 lines) is the entire integrity checker in one function. This matters more
than its size suggests: fsck is the backstop that both known correctness gaps point at. Target
units: universe enumeration, per-object classification (pure), the snapshot oracle (already
separate), the reachable-but-absent scan, and report assembly.

### B3 — `Pool/CasRefLedger.cpp`

The stateful, concurrent remainder left behind when `CasRefProtocol` was extracted. Target units:

| Unit | Owns |
|---|---|
| Carve + admission | two-phase PLAN/PUBLISH under one queue-lock hold, dedup by ref name, batch cap, solo scope for `WholeShard` and non-`Live` namespaces |
| Validation + chunking | op and byte caps, shape check, admission budget, fail-alone semantics, chunk cut and reseed |
| Wedge protocol | prepare the complete wedge before the PUT, arm apply-pending, the four outcome classes, resolve-before-allocate on the next flush |
| Install region | noexcept, allocation-free swap plus counter updates |
| Recovery orchestration | error classification, backoff budget, bounded restart-on-vanish, the recovery seal, install-last ordering |
| Snapshot cadence | admission predicate, one-in-flight, count/byte thresholds, chunked-tenure re-evaluation, backoff |
| Stale-precommit sweep | epoch-based selection, chunking, retry-until-verified-clean, read-path insulation |
| Table cache eviction | LRU with quiescence and wedge guards |

`confirmExactRef` (108 lines, self-contained, already has its own gtest) stays as it is.

### B4 — `Pool/CasPartWriteTxn.cpp`

- **Blob upload state machine** — the dedup gate, conditional create, adopt, and the
  displace/resurrect path. `uploadFromSource` (354) is one algorithm and should read like one.
- **Promote's leaf revalidation** — the trusted/tokened/evidence rules as a pure predicate set.
- **Abandon and debris cleanup** — including the precommit-state ordering rules.

### B5 — `Gc/CasBlobInDegree.cpp`

Separate the run encode/decode and streaming from `foldDeltasIntoGeneration` (282 of 665 lines).
The settlement half is already close to pure; finish it and give it direct tests.

### B6 — `Pool/CasPool.cpp`

66 methods on one class. Split by role — mount lifecycle, ref delegation, plain objects, blob
upload, staging, event dispatch — keeping `Pool` as a thin facade so call sites do not churn. This
is the highest-risk item in workstream B because everything depends on `Pool`; do it after B1–B5,
when the units it delegates to are already extracted.

### B7 — `ContentAddressedTransaction.cpp`

Mechanical and cheap: the two write buffers to their own files, `PartStaging` to its own, and the
two stray `ContentAddressedMetadataStorage` methods moved home.

### B8 — `ContentAddressedMetadataStorage.cpp`

Extract the `IContentAddressedExchange` relink implementation to its own file — it is a network
protocol, not a metadata-storage concern — and extract path routing plus directory classification.

---

## 6. Workstream C — test re-seaming

### 6.1 The rule

> **A test lands in the pull request that introduces the topmost unit it genuinely exercises.**

Three categories follow:

- **Pure unit tests** land with their unit. Workstream B *grows* this category: purifying decision
  logic makes it testable with no backend at all.
- **Unit-against-a-double** tests land with their unit, provided the double's layer is already in.
  `Backend/CasInMemoryBackend.cpp` is the key enabler and arrives with the Backend layer.
- **Assembly tests** that genuinely span layers belong to the topmost layer. This is not a
  compromise: they test the assembly, so they belong to the PR that completes it.

The rule doubles as a design check. **If a test you believe is a unit test needs five layers, that
is a bad seam in the code, not a packaging problem** — either it is really an assembly test, or the
unit has a dependency it should not have. Test re-seaming and code decomposition therefore proceed
together and validate each other.

### 6.2 The root cause to fix first

`src/Disks/tests/cas_test_helpers.h` is **1250 lines, included by 73 of ~101 test files, and pulls
in five layers** (`Primitives`, `Formats`, `Backend`, `Pool`, `Gc`). Every test that includes it
becomes five-layer by construction, whatever it actually tests. Its contents are already stratified
and split along the same seams as the code:

| Fixture layer | Examples |
|---|---|
| Layer-agnostic | `expectThrowsCode`, `hexOf`, `streamingHexOf`, `u128Of`, `makeLocalObjectStorageForTest` |
| Primitives | `idOf`, `blobEntryFor` |
| Backend | `writeBlobRaw`, `writeBlobBody`, `blobAbsent`, `displaceObjectToken`, `writeMetaClean` |
| Formats / ref protocol | `writeRefLogTxnRaw`, `appendOwnerEvent`, `promoteTransition`, `addPrecommitTransition`, `writeManifestRaw`, `deleteManifestBody` |
| Pool | `openPoolForTest`, `makeSettingsForTest`, `seedPoolMetaForRestart` |
| GC | `encodeMinimalGcState`, `injectRetire`, `injectCondemnedSummarySeal`, `currentRetiredSet`, `runRoundsUntilAbsent` |

Splitting this header into per-layer fixtures is the single highest-leverage preparatory action for
the test suite, and it must happen **before** the big test files are split.

### 6.3 Test files to split

The five largest, along the same seams as the code they cover:
`gtest_cas_ref_writer.cpp` (3731), `gtest_cas_part_write.cpp` (2501), `gtest_cas_pool.cpp` (1916),
`gtest_cas_gc_round.cpp` (1629), `gtest_cas_ref_statemachine.cpp` (1422).

`gtest_ca_wiring.cpp` (2873, six layers) is an assembly test by nature. It stays whole and lands
last, with the wiring PR.

### 6.4 Enforcement

Add a cheap check that computes each test file's layer reach from its includes and compares it to a
declared layer. A test whose reach exceeds its declaration fails the check. This prevents the
monolithic-fixture problem from silently returning during the split.

---

## 7. Workstream D — the target series

Landing order for the subsystem, after B and C are complete. Each PR compiles, passes its own
tests, and changes no behaviour.

1. `Primitives` + `Formats` (breaking the one `CasTypes.h → CasFormat.h` cycle) + the format battery
2. `Backend` — interface, object-storage adapter, in-memory backend, capability probe, request control
3. `Pool` identity — `CasServerRoot`, `CasMountRuntime`, `CasPoolMeta`, `CasPlainObjects`
4. Ref subsystem — the units of B3, as a mini-series (§7.1)
5. Writer — the units of B4, plus `CasBlobUploadPool`, `CasManifestReader`, `CasBlobMeta`
6. `Parts` — `PartPathParser`, `PartFolderAccess`
7. GC — the units of B1 and B5, as a mini-series
8. `Tools` — fsck (units of B2), inspect, decommission, and the `programs/disks` commands
9. Wiring — metadata storage, transaction, exchange, settings, system tables, **the registration
   line**, and the stateless/integration tests

**Parallelism.** The no-stacked-PR rule forces *dependent* changes to land sequentially, but the
extracted units are new files that do not depend on one another — the wedge protocol knows nothing
about fold stages, snapshot cadence knows nothing about carve. Many unit PRs can therefore be open
simultaneously against the base branch and merge as they are reviewed. Only the per-layer
assembly PRs and the final wiring PR are genuinely serialised.

**Documentation.** `docs/superpowers/` (344 377 lines of worklogs, models and reports) does **not**
ship. Only the curated `docs/superpowers/cas/01`–`11` set travels, as its own PR.

---

## 8. Mechanics — how the series is produced

The existing history (3060 commits) is not usable and will not be rewritten. Instead:

1. Complete workstreams B and C **on the working branch**, gated per step (§9). The result is a
   clean final tree.
2. Create a fresh branch from `altinity/antalya-26.6`.
3. Add units in dependency order, one commit (and one PR) per unit, each with its tests.

Step 3 is mechanically straightforward **because the subsystem is purely additive** — 0 deletions
in the CAS directory — so there is no interleaving with existing code to reconstruct.

**Doing B and C before carving is deliberate.** Refactoring after units have landed would mean
re-reviewing already-merged code and paying for extra PRs; and while the series is in flight the
working branch would diverge from what has landed.

**The completeness invariant.** When the series is finished, the diff between the series tip and
the refactored working branch, restricted to the shipped paths, must be **empty**. This is
checkable with a single `git diff` and is the proof that carving lost nothing.

---

## 9. Verification

**Per extraction step (workstream B):**

- the full CAS gtest gate must pass. Note that the obvious filter `Cas*:CA*` **under-selects**:
  it has twice been found to miss whole families (`RefWriter*` and friends, then `CaWiring*` and
  friends — the second omission was hiding three real bugs). The gate must enumerate the families
  explicitly, and the decomposition will add new ones, so the filter is updated with each step;
- a soak run must pass. **A gtest-only gate is not sufficient**: the correctness of the ref lane
  and the GC round rests on lock discipline and ordering that unit tests do not observe. An
  extraction that silently changes when a lock is held will pass gtests and fail in production.

**Per unit PR (workstream D):** the PR's own tests, plus a build.

**For the series as a whole:** the completeness invariant of §8, plus a full stateless and
integration run on the CA-default lane at the wiring PR.

---

## 10. Risks

**R1 — an extraction changes lock discipline.** The publish lane's correctness depends on the
two-phase carve happening under one continuous lock hold and on the install region not allocating.
Mitigation: extract *pure* functions called from the same place under the same lock, so the
discipline is unchanged by construction; and gate on soak, not just gtests. This is the top risk of
the effort.

**R2 — `Pool` facade churn.** B6 touches the class everything depends on. Mitigation: do it last,
keep `Pool` as a delegating facade so call sites are untouched.

**R3 — the walkthrough goes stale.** `docs/superpowers/cas/11-walkthrough.md` cites roughly 200
`file:line` references, and workstream B invalidates most of them. Mitigation: refresh it as the
final step of B, before carving. It is the document reviewers will read first, so a stale one is
worse than none.

**R4 — upstream and fork diverge on workstream A.** A3 may be reshaped in upstream review after it
has already landed in the fork. Mitigation: keep A3's fork commits identical to what is proposed
upstream, and treat upstream feedback as a follow-up to both.

**R5 — calendar.** Roughly 20–25 PRs. Mitigation: the parallelism of §7; and the unit PRs are small
(200–600 lines), so review latency per PR is low.

---

## 11. Open decisions

- **D1 — granularity.** Proposed: fine-grained, one complex algorithm per PR (~20–25 PRs), relying
  on the parallelism of §7. The alternative is eight medium PRs of ~5–6k lines each.
- **D2 — sequencing of B.** Proposed: complete all of B and C before carving, per §8. The
  alternative — carve each hot spot as soon as it is extracted — gets reviewer feedback earlier but
  keeps the working branch alive longer and risks divergence.
- **D3 — where the extracted units live.** Whether to add sub-directories (for example
  `Gc/Round/`, `Pool/Ref/`) or keep the current flat per-layer layout with more files.
