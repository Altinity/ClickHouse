# B194 — `GcSnap::stripTree` O(N×M) → O(children) via a reverse index — spec + plan

**Status:** design + plan (2026-06-26). Branch `cas-vfs-path-mapping`. **Behavior-preserving** perf fix; no wire/format change. Pinpointed as the #1 on-CPU GC frame by live `trace_log` (attempt-4 soak: `GcSnap::stripTree <- Gc::cascadeAndPersist`, 162 Real samples, > S3-I/O-wait).

## Problem
`GcSnap::stripTree(parent_tree)` (`CasGcSnap.cpp:190`) full-scans the entire per-shard `edges` map (`std::map<String, EdgeRec>`) erasing edges whose `parent_tree == X`. `Gc::cascadeAndPersist` calls it **once per cascaded tree deletion**, so a round deleting N trees over a shard holding M edges is **O(N×M)** — quadratic. The big soak rounds cascade 100 K–182 K children → quadratic CPU dominates GC.

## Key invariant that makes the fix simple (grounded)
Tree edges (`edge_kind != Root`, carry `parent_tree`) enter `edges` **only** via `addEdge` (`:101`, on `try_emplace` `inserted`) and leave **only** via `stripTree` (`:199`). Root edges (`addRootEdge`/`removeRootEdge`, `:115`/`:178`) carry no `parent_tree` and are skipped by `stripTree`. So a reverse index needs maintenance at exactly **two** sites.

## Design
Add a derived member to `GcSnap`:
```cpp
std::map<UInt128, std::vector<String>> children_by_tree;  // parent_tree -> tree-edge ids
```
(UInt128 is ordered — `std::set<UInt128> expanded` already exists.)

1. **`addEdge`** — capture `is_tree = rec.edge_kind != EdgeKind::Root` and `parent = rec.parent_tree` BEFORE the `std::move`; after `try_emplace`, if `inserted && is_tree`, `children_by_tree[parent].push_back(id)`. (Only on `inserted` — addEdge is set-semantics, so a duplicate add must NOT double-list.)
2. **`stripTree`** — replace the full scan with an index lookup:
   ```cpp
   std::vector<Candidate> GcSnap::stripTree(const UInt128 & parent_tree) {
       std::vector<Candidate> result;
       if (auto cit = children_by_tree.find(parent_tree); cit != children_by_tree.end()) {
           for (const String & id : cit->second) {
               auto it = edges.find(id);
               if (it == edges.end()) continue;   // defensive
               if (auto c = dropEdgeTarget(it->second)) result.push_back(*c);
               edges.erase(it);
           }
           children_by_tree.erase(cit);
       }
       expanded.erase(parent_tree);
       return result;
   }
   ```
   O(children-of-parent), and it erases the whole parent entry at once → no per-edge index bookkeeping on the erase side.
3. **`decodeSnapFields`** (`:299`) rebuilds `edges` via `addEdge` (`:334`) → `children_by_tree` is reconstructed for free. **`encodeSnapFields` does NOT serialize it** (derived) → **no wire/format change** (golden snap bytes unchanged).

## Behavior-preserving argument
The new `stripTree` visits exactly the tree edges with `parent_tree == X` (the index lists precisely those, by the two-touch-point invariant), calls `dropEdgeTarget` + `edges.erase` on each, and clears `expanded[X]` — identical observable effect to the old scan (same Candidates, same final `edges`/`indeg`/`known`/`expanded`). Order of Candidates may differ (index order vs map order) — callers treat the result as a set (a retire candidate list), so order is immaterial; the test should compare as sets.

## Plan (TDD, one commit)

### Task 1: failing/again test → reverse index → green
**Files:** `Core/CasGcSnap.h` (member), `Core/CasGcSnap.cpp` (`addEdge` + `stripTree`), `src/Disks/tests/gtest_cas_gc_snap.cpp` (or the closest gc-snap test — grep `stripTree`/`addTreeEdge` in tests).

- [ ] **Step 1 — consistency + equivalence test:** build a snap with one root→treeA edge + many treeA→child edges (and an unrelated treeB with its own children, + some root edges), via `addRootEdge`/`addTreeEdge`. Assert: (a) `stripTree(treeA)` returns the SAME candidate set as the pre-fix scan would (compare as a sorted set of (kind,hash)); (b) after strip, `edges` contains exactly treeB's edges + root edges (treeA's gone); (c) `indeg`/`known` match; (d) `children_by_tree` no longer has treeA and still has treeB's full list; (e) **encode→decode round-trip** then `stripTree(treeB)` works (proves the index rebuilds on decode). Run → confirms behavior.
- [ ] **Step 2 — implement** the `children_by_tree` member + the `addEdge` append (on `inserted && tree`) + the index-driven `stripTree`.
- [ ] **Step 3 — build + test:** `cd build && ninja unit_tests_dbms > cas_b194_build.log 2>&1` (no -j; read tail). Run the new test + full `--gtest_filter='Cas*:Ca*'` → only the baseline red `CaWiringOps.FreezeViaHardLinksIntoShadow`. The existing gc-snap golden/encode-decode tests MUST still pass (proves no wire change). **Do NOT `ninja clickhouse`** (a soak is running on the mounted binary).
- [ ] **Step 4 — commit:** `CA B194: GcSnap::stripTree O(N×M)->O(children) via parent_tree reverse index (rebuilt on decode)`.

## Verification (done)
Build clean (`-Werror`); the equivalence+consistency+decode-rebuild test passes; golden snap codec tests unchanged (no wire change); sweep baseline-only red. Re-soak (after `ninja clickhouse`) should show `stripTree` drop out of the top GC `trace_log` frames (the before baseline is attempt-4's 162-sample `stripTree` stack).
