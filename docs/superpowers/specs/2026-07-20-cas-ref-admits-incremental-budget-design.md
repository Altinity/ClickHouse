# CAS ref-ledger `admits()` — incremental budget accounting

- **Date:** 2026-07-20
- **Status:** design approved, ready for implementation plan
- **Area:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (ref ledger / ref protocol)
- **Backlog item:** `utils/ca-soak/scenarios/BACKLOG.md` — *"admits() re-encodes the WHOLE
  ref table once per state-growing op in a flush batch"* (logged 2026-07-19)

## Problem

`admits(state, op, snapshot_budget, removal_budget)` (`Pool/CasRefProtocol.cpp:325-341`)
answers *"would applying this one op push either budget-relevant encoding over its byte
limit?"* by doing a **full O(N) rebuild + encode from scratch on every call**, where N is
the namespace's total live ref count:

1. `snapshotOf(scratch, "")` iterates the entire merged `RefCowMap`, then
   `encodeRefTableSnapshot` serializes the whole table — just to read `.size()`.
2. `buildHypotheticalRemovalTxn(scratch, ...)` iterates the whole `committed` + `precommits`
   set to build a whole-namespace removal transaction, then `encodeRefLogTxn` encodes that in
   full — just to read `.size()`.

`admits()` is called **once per state-growing op** inside the per-item loop in
`CasRefLedger::flushRefBatch` (`Pool/CasRefLedger.cpp:1096-1116`). A flush batch with K
state-growing ops against a table with N live refs therefore costs **O(K×N)**.

### Measured impact

- Standalone micro-benchmark `BM_Admits`
  (`benchmarks/benchmark_cas_ref_protocol.cpp`, Google Benchmark, `-DENABLE_BENCHMARKS=ON`):

  | N | time/call |
  |---:|---:|
  | 100 | 48.8 μs |
  | 1,000 | 476 μs |
  | 10,000 | 5,018 μs |
  | 100,000 | 55,976 μs |

  Google Benchmark's complexity fit across the range: **O(N log N)**, RMS 2% (the log-N term
  is consistent with `RefCowMap`'s `std::map`-backed merged iteration).
- 5h soak `system.trace_log` `CPU`: `RefCowMap::const_iterator::operator++()` hot inside
  `admits()`.
- 5h soak `system.trace_log` `Real`: committer threads blocked in `pthread_cond_wait` inside
  `CasRefLedger::appendRefOps` — the O(N) cost queues concurrent committers.
- 5h soak `system.events`: `CasRefQueueWaitMicroseconds / CasRefBatchedMutations` ≈ **~453 ms
  average caller wait per ref-op**, near-identical on both replicas — a direct latency tax on
  every committed part, mutation, or removal.

## Key structural insight

**Both budget-relevant encodings are pure per-row sums.** Each format is line-oriented text
in which every row / op encodes *independently* of every other row — no delta-coding, no
shared-prefix compression, no cross-row state:

- **Snapshot** (`Formats/CasRefSnapshotFormat.cpp`):
  `header + meta-line + Σ writeCommittedRow(row) + Σ writePrecommitRow(pc) + trailer(count)`.
  `writeCommittedRow` reads only that row's own fields.
- **Hypothetical removal txn** (`Formats/CasRefLogFormat.cpp`):
  `header + meta-line + Σ removalOp(committed row) + Σ removalOp(precommit) +
  remove_namespace-op + trailer(count)`. Each removal op's bytes depend only on
  `(owner_kind, ref_name, manifest_ref)` of its row.

So the encoded size is an **exact linear functional of the row set**: a sum of independent
per-row contributions plus O(1) framing (header/meta/trailer, where trailer size is a function
of the row count).

**Why this reverses the current design comment.** The header comment at
`Pool/CasRefProtocol.h:263-268` deliberately chose the full re-encode, justified as *"This can
never drift from what those encoders actually produce (there is nothing to keep in sync)."*
That reasoning assumed an incremental counter would be a **separate hand-rolled estimate**
(re-implementing string escaping / length math) that could diverge. Because the format is a
pure per-row sum, we can instead keep a running **body-byte total** that is **byte-identical**
to the full encode — computed by feeding each *touched* row through the *same* codec
primitives. It is the same bytes summed in a different order; there is nothing to drift.

**The mutation choke point is tiny and fully contained.** Every row-level change to
`committed` / `precommits` happens in one file (`Pool/CasRefProtocol.cpp`) at 5 sites inside
`applyOwnerTransition` / `applySetPayload`, plus 1 seeding site in `stateFromSnapshot`.
`applyOpInPlace` is the sole mutator.

## Design (Approach A — incremental body-byte counters in the state machine)

### 1. Data model

Add two scalars to `RefTableState` (`Pool/CasRefProtocol.h`):

```cpp
uint64_t snapshot_body_bytes = 0;   // Σ committed-row line sizes + Σ precommit-row line sizes
uint64_t removal_body_bytes  = 0;   // Σ removal-op line sizes (one per committed + one per precommit)
```

- **Body sums only** — they exclude the O(1) framing (header / meta / trailer, and the removal
  txn's terminal `remove_namespace` op). Framing is recomputed on demand.
- A **pure function of `(committed, precommits)`**, so value-copy semantics are automatically
  correct: every `RefTableState` copy (`item_scratch`, `shape_check`, `admits`' scratch) carries
  consistent totals for free — two 8-byte fields, trivial next to the COW `committed`.
- **Invariant:** the only ways to populate `committed` / `precommits` are `stateFromSnapshot`
  and `applyOpInPlace`, so those are the only two places that seed / maintain the totals.

### 2. Single source of truth for per-row & framing sizes (Formats layer)

Factor small size helpers out of the two encoders, each implemented by writing exactly the same
fragment the full encoder writes into a throwaway `WriteBufferFromOwnString` and returning
`.size()` — **no hand-rolled length / escaping math**, so they cannot diverge from the real
encoder. The full encoders are refactored to call the same building blocks (one implementation
per fragment).

Snapshot format (`Formats/CasRefSnapshotFormat.h/.cpp`):

- `size_t committedRowEncodedSize(const RefCommittedRow &)` — O(row length).
- `size_t precommitRowEncodedSize(const RefOwnerBinding &)` — O(row length).
- `size_t snapshotFramingSize(ns, snapshot_id, lifecycle, remove_txn_id, sealed_from, row_count)`
  — O(1).

Log format (`Formats/CasRefLogFormat.h/.cpp`):

- `size_t removalOpEncodedSize(RefOwnerKind, const String & ref_name, const ManifestRef &)`
  — the removal `owner_transition` op line for one owner. O(row length).
- `size_t removalFramingSize(ns, txn_id, op_count)` — header + meta + `remove_namespace` op +
  trailer. O(1).

### 3. Counter maintenance — the 5 sites + seeding (`Pool/CasRefProtocol.cpp`)

Each site already holds the affected row (from `find` / `erase` / `emplace`), so updates are
local — no extra scan. Updates happen *after* the op's preconditions pass, alongside the row
mutation.

| Site (line) | `snapshot_body_bytes` | `removal_body_bytes` |
|---|---|---|
| add precommit (56) | += precommitRowSize(b) | += removalOp(Precommit, b) |
| remove precommit (64) | −= precommitRowSize(b) | −= removalOp(Precommit, b) |
| remove committed (78) | −= committedRowSize(old) | −= removalOp(Committed, old) |
| promote (91, 107) | −= precommitRowSize; += committedRowSize(empty payload) | −= removalOp(Precommit); += removalOp(Committed) |
| set_payload (133–136) | −= committedRowSize(old); += committedRowSize(updated) | net 0 (ref_name / manifest_ref unchanged) |

- `stateFromSnapshot` (229/231) accumulates both totals during its existing build loop — cold
  path, once per recovery.
- Default / empty state = 0. `namespace_birth` / `remove_namespace` touch no rows → no counter
  change (a `remove_namespace` only fires once both sets are already empty, so both totals are
  already 0 at that point).

### 4. New `admits()` — no delta math

```cpp
bool admits(const RefTableState & state, const RefOp & op,
            uint64_t snapshot_budget, uint64_t removal_budget)
{
    static constexpr RefTxnId kPreviewTxnId{1, 1};
    RefTableState scratch = state;
    applyOpInPlace(scratch, op, kPreviewTxnId);   // unchanged; throws on illegal op exactly as today
    const uint64_t rows = scratch.committed.size() + scratch.precommits.size();

    if (snapshotFramingSize("", scratch.greatest_applied, scratch.lifecycle,
                            scratch.remove_txn_id, /*sealed_from*/std::nullopt, rows)
        + scratch.snapshot_body_bytes > snapshot_budget)
        return false;

    return removalFramingSize("", kPreviewTxnId, rows + 1)
        + scratch.removal_body_bytes <= removal_budget;
}
```

This reproduces today's preview exactly: `snapshotOf` uses `snapshot_id = state.greatest_applied`
(untouched by `applyOpInPlace`), `sealed_from` unset, ns empty; the removal txn id is `{1,1}`
and its op count is `rows + 1` (one removal op per owner plus the terminal `remove_namespace`).
Cost drops to O(touched rows) ≈ O(1). Flush batch: **O(K×N) → O(K)**.

### 5. Anti-drift safety net (what earns the byte-exact guarantee)

- **Debug `chassert`:** after each `applyOpInPlace`, recompute both body sums from scratch
  (O(N), debug builds only) and assert equality with the running totals. Continuously *proves*
  the incremental path equals ground truth — the direct rebuttal to the old "it can drift"
  rationale.
- **Fuzz / property gtest** (in the existing `gtest_cas_ref_*` suite): random legal op
  sequences; after each op assert
  `snapshotFramingSize + snapshot_body_bytes == encodeRefTableSnapshot(snapshotOf(...)).size()`
  and the removal analogue. This is the definitive byte-exactness proof.

### 6. Validation & scope

- All existing ref-protocol / intake gtests stay green (behavior identical).
- Re-run `BM_Admits` to confirm flat scaling across N = 100…100,000 (was O(N log N)); this also
  finally gives `BM_EncodeRefLogTxn` its before/after diff.
- **No wire-format change, no persisted-data change** → no `NativeFormat` spec impact, and no
  compat scaffolding (pre-release rule).
- Update the `Pool/CasRefProtocol.h:263-268` header comment: the "non-incremental, so it can't
  drift" rationale is replaced by "incremental body sums, kept byte-exact and continuously
  validated."

## Decisions & alternatives considered

- **Correctness bar: byte-exact, identical admit/reject decision** (chosen) — a pure perf
  refactor with zero behavior change, backed by the §5 safety net. Rejected a "safe conservative
  over-estimate" as unnecessary once byte-exactness is provable.
- **Counters on `RefTableState`** (chosen) vs. pushing the committed-side sum into `RefCowMap`
  next to `net_delta`: the removal sum spans both `committed` and `precommits`, so a single owner
  is cleaner than splitting across two containers.
- **Framing recomputed fresh per `admits`** (O(1)) rather than cached — not worth caching for
  constant work.
- **Approach B (cache & splice the full encoded strings)** — rejected: holds two full encoded
  strings per table and needs fiddly per-line splicing, for no benefit over summing sizes.
- **Approach C (reduce `admits()` call frequency — backlog fix #2)** — rejected: each remaining
  call is still O(N), and it changes per-op validation granularity (a later op in an item is
  currently validated against a state reflecting earlier ops), which needs its own semantics
  review. Lateral on cost, weaker on safety.

## Risks

- **A mutation site added later that bypasses `applyOpInPlace`** would desync the counters. The
  debug `chassert` catches this immediately in tests; the invariant note in §1 documents the
  contract. `RefCowMap` iterators are already read-only, so an in-place row edit is not possible.
- **`RefTableState` equality / hashing in tests:** the counters are a pure function of the rows,
  so any two states with equal rows have equal counters. Verify no test constructs a
  `RefTableState` with populated rows outside the two seeding paths.
