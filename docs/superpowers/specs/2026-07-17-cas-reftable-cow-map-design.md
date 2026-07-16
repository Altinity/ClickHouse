# CAS ref-table `committed` copy → O(touched) via a COW overlay map — design

**Status:** design approved (brainstorming), pending spec review → `writing-plans`.
**Branch:** `cas-gc-rebuild`. **Scope:** `RefTableState::committed` only. Pure performance change — behavior byte-identical.

## Motivation {#motivation}

Under a commit/mutation-heavy workload the #1 CPU stack (TXN-Final soak `trace_log`) is `std::__tree::__copy_construct_tree` — deep-copying `RefTableState::committed` (`std::map<String, RefCommittedRow>`, `Pool/CasRefProtocol.h:148`). `CasRefLedger::flushRefBatch` copies the whole map per batch item — `item_scratch = working` (`CasRefLedger.cpp:1060`), `shape_check = working` (`:1078`) — and `applyRefLogTxn`/`admits` are themselves copy-mutate-swap (`scratch = state`, `CasRefProtocol.cpp:272`, `:325`), which fire repeatedly as trials, **compounding** on top of the two explicit copies. A typical batch item touches ~1 ref (the carve at `:1026` guarantees distinct ref_names), so copying the whole map per item/op is the waste. Goal: make a copy **O(touched rows)**, not O(all refs).

## Facts that constrain the design (from the access map) {#constraints}

- **Hot path is all keyed:** `resolveRef` (`CasRefLedger.cpp:129`, `committed.find`) and every `build_ops` reader (`dropRef:1768`, `updateRefPayload:1811`, publish/promote/precommit `CasPartWriteTxn.cpp:842/931/1011`) are keyed `find`. Full scans are cold: `listRefs:171`, `snapshotOf` (`CasRefProtocol.cpp:288`), solo `dropNamespace:1880` (always a batch=1 WholeShard carve).
- **Ordered iteration is load-bearing:** `snapshotOf`'s canonical (bytewise-sorted-by-ref_name) output and removal-txn build require **sorted** iteration. So an unordered overlay is insufficient; the merged view must iterate in sorted order.
- **Isolation invariant (must preserve exactly):** the leader takes a stable copy of `rt->state` under `state_mutex` (`:1006`), then validates + does the network PUT **without** the lock, re-acquiring only to install the result (`applyRefLogTxn(rt->state,…)` `:1214-1215`); readers (`resolveRef`/`listRefs`) hold `state_mutex` for the whole in-place read; the detached snapshot task copies `candidate_state = rt->state` under lock then encodes+PUTs outside (`:1473`, owner pinned via `pin_owner`).
- **`std::map` is a plain field**, never crossing a signature as `std::map` (codec uses `std::vector<RefCommittedRow>`); ~15 access sites; ops used = `find`/`end`/`contains`/`erase(key)`/`erase(it)`/`emplace`/`size`/`empty`/ordered structured-binding iteration. No `lower_bound`/range, no iterator-stability-across-mutation reliance.

## Mechanism — COW overlay over an immutable shared ordered base {#mechanism}

Introduce a value-semantic ordered-map type (working name `RefCowMap`) replacing `std::map<String, RefCommittedRow> committed`:

- Internals: `std::shared_ptr<const std::map<String, RefCommittedRow>> base` (immutable, shared) + `std::map<String, std::optional<RefCommittedRow>> overlay` (present = inserted/updated row, `nullopt` = tombstone).
- **Copy = O(1):** share the `base` pointer (atomic refcount bump) + copy the small `overlay`. This is what makes every copy site — `working`, `item_scratch`, `shape_check`, `applyRefLogTxn`/`admits` internal `scratch`, `candidate_state` — O(touched-this-flush) instead of O(all refs).
- **Keyed read (`find`/`contains`/`at`):** overlay first (present → hit; tombstone → miss); else `base`. O(log touched + log n).
- **Point write (`emplace`/`insert_or_assign`/`erase`):** write to `overlay`. O(log touched).
- **Ordered iteration:** merge-iterate `base` and `overlay` in sorted order, applying overlay overrides/tombstones — a standard two-sorted-range merge. Used only on cold paths (`snapshotOf`, `listRefs`, solo `dropNamespace`). O(n + overlay).
- **`size`/`empty`:** tracked incrementally (base size ± overlay inserts/tombstones) so it stays O(1).

**Reuse / prior art (per codebase search):** this is not novel here — it is the pattern Keeper already uses (`KeeperStorage::UncommittedState` + `Delta`/`applyDelta`/`rollback` over a committed `Container`, `KeeperStorage.h:522`), specialized to our *ordered* map. The immutable-base + atomic-swap building block is `MultiVersion`/`std::shared_ptr<const T>`. `src/Common/COW.h` is whole-object COW (clone the whole map on any write) — that is the rejected "approach C" (trial copies always write → no gain). A persistent/immutable ordered tree ("approach A", O(1) copy + no per-flush materialize) has no drop-in implementation in-tree and would be hand-rolled — deferred as a future optimization if the once-per-flush materialize proves insufficient.

## Materialization — eager on install {#materialization}

When a flush installs the final state under `state_mutex` (after the batch's `applyRefLogTxn(rt->state, final_txn)`), **materialize** `rt->state.committed` (fold `overlay` into a fresh immutable `base` map, `overlay` emptied) — O(n) **once per flush**, versus today's O(n) × items × ops. Between flushes `rt->state.committed` is `base` + empty `overlay`, so ordinary keyed reads (`resolveRef`) and full scans hit `base` directly with no merge overhead. Trial copies during a flush share the base and carry only the batch's small growing overlay.

## Isolation — unchanged invariant {#isolation}

`base` is `shared_ptr<const map>` → immutable, safe to share across threads; a copy is a refcount bump (thread-safe) plus the overlay copy. The leader's `working` is a private value (own overlay, shared base) — it can validate + PUT without `state_mutex` exactly as today. Install swaps `rt->state`'s base/overlay under `state_mutex`. Readers hold `state_mutex` for the whole in-place read as today; any full-scan merge happens under that lock. The detached snapshot task copies (O(1)) under lock and encodes outside, as today. No new locking; the exact invariant the access map identified is preserved.

## Scope / ripple {#scope}

- `RefTableState::committed` only. `precommits` stays `std::set` (small; not a hot copy cost).
- The ~15 access sites use `RefCowMap`'s `std::map`-compatible subset drop-in; `snapshotOf`/`listRefs`/`dropNamespace` use the ordered merge-iterator. The codec boundary is unchanged (still `std::vector<RefCommittedRow>`, built by iterating the merged view).
- Base container is `std::map` first (minimal change); swapping the base to `absl::btree_map` (in-tree, ordered, better copy/iterate constant) is a trivial, optional later change — not in this cut.

## Correctness & testing {#testing}

Behavior is byte-identical (pure perf): the all-or-nothing copy-then-swap in `flushRefBatch` and the two-phase `scratch`-swap in `applyRefLogTxn` work unchanged — only the copy cost drops.

- **Property/fuzz gtest: `RefCowMap` ≡ `std::map`** — random op sequences (insert/update/erase/find/size/ordered-iteration), including copy-then-mutate isolation (mutating a copy must not affect the original or the shared base) and tombstone/override correctness on the merged iterator.
- **O(1)-copy assertion:** a copy does not deep-copy the base (e.g. base `shared_ptr use_count` increments; no per-row allocation) — a direct test of the actual win.
- **Existing `Ca*:Cas*` battery stays green** (905/905) — behavior identical.
- **`snapshotOf` byte-identical:** the canonical snapshot bytes for a given state are unchanged (ordered merge produces the same sorted output).
- **Commit-path evidence:** a short soak / `trace_log` check that `__copy_construct_tree` of the ref-table map is no longer a top CPU stack; optionally a microbench of `flushRefBatch` on a large table.

## Non-goals {#non-goals}

- Persistent/immutable ordered tree (approach A) — deferred; revisit only if the once-per-flush materialize is itself too costly.
- `absl::btree_map` base — optional constant-factor follow-up, not this cut.
- `precommits` / any other `RefTableState` field — unchanged.
