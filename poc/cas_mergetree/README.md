# Content-Addressed Shared MergeTree — Proof of Concept

A **standalone, self-contained C++20 PoC** that validates the core algorithms and data
model of the design in
[`../../content_addressed_shared_mergetree_design.md`](../../content_addressed_shared_mergetree_design.md)
(v3). It is **not** integrated into ClickHouse — it deliberately reimplements just enough of
the part lifecycle to prove the architecture end-to-end with executable tests.

## What it demonstrates

| # | Property | Test |
|---|---|---|
| 1 | Write a part as content-addressed objects, read it back identically | `test_roundtrip` |
| 2 | **Blob dedup** — an identical column across two parts is stored once | `test_blob_dedup` |
| 3 | **Idempotent** content-addressed upload (retry / identical content is free) | `test_idempotent` |
| 4 | **Mutation carry-forward by reference** — change 1 of N columns ⇒ 1 new blob, the rest reused | `test_carry_forward` |
| 5 | **Covering** — a merge supersedes its sources via the `MergeTreePartInfo` name rule | `test_covering` |
| 6 | **DROP via a tombstone covering ref** (removal supersession) | `test_drop_tombstone` |
| 7+8 | **Lifecycle + reachability GC** — outdated refs removed, then unreachable objects collected after grace; the merged part survives | `test_lifecycle_then_gc` |
| 9 | GC keeps blobs shared by carry-forward, collects only the replaced one | `test_gc_keeps_carry_forward` |
| 10 | GC **never** deletes blobs of an active part, at any age | `test_gc_never_deletes_reachable` |
| 11 | **Ephemeral reader pin** keeps a dropped part's blobs alive across "nodes" (the stateless-reader fence) | `test_reader_pin` |
| 12 | DETACH/ATTACH — detached is a reachability root, not active, still readable, not GC'd | `test_detach_attach` |
| 13 | **FREEZE materializes real bytes** — the snapshot survives drop + GC of the originals | `test_freeze_materializes` |
| 14 | Two byte-identical parts share **one** manifest object (cross-replica/table dedup by reference) | `test_manifest_dedup` |

These map directly to the v3 design and to the fixes from the two adversarial review rounds:
content-addressing (§1–2), the refs/covering active set + tombstone removal (§3), reachability
GC gated by the outdated-parts lifecycle with grace-from-loss-of-reachability (§5), the ephemeral
reader pin for stateless compute (§5/§7), carry-forward (§1/§4), and FREEZE-materializes-bytes (§4/§8).

## Build & run

```bash
cd poc/cas_mergetree
cmake -S . -B build -G Ninja
cmake --build build
./build/cas_tests          # prints a trace; exits 0 iff all checks pass
# or: ctest --test-dir build --output-on-failure
```

Requires a C++20 compiler (tested with g++ 14.2) and CMake ≥ 3.16. No third-party deps.
Scratch object stores are created under `./cas_poc_scratch/` (one dir per scenario) and can be deleted.

## Structure

- `cas.h` / `cas.cpp` — the model:
  - `Hash128` / `hash128` — deterministic 128-bit content hash (stands in for `cityHash128`).
  - `ObjectStore` + `LocalObjectStore` — dumb content-addressed key/value over a directory (stands in for `IObjectStorage`/S3); tracks put/get/remove counters for assertions.
  - `PartInfo` — a faithful subset of `MergeTreePartInfo`, including the `contains` **covering rule** and **tombstone** parts.
  - `Manifest` — content-addressed part metadata (blob-hash footer + inline small files); canonical serialization ⇒ stable hash.
  - `Ref` / `Catalog` — `part_name → manifest_hash` refs persisted in the object store; `ActivePartSet` derives the active set by covering.
  - `Engine` — `insertPart` / `mergeParts` / `mutatePart` (carry-forward) / `dropRange` (tombstone) / `detach` / `attach` / `freezePart` (materialize) / `readPart`; the outdated-parts **lifecycle** (`removeOutdatedRefs`); **ephemeral reader pins**.
  - `GC` — reachability mark-and-sweep over `blobs/` and `manifests/`, grace measured from first loss of reachability.
- `tests.cpp` — the scenarios above (a tiny assert harness; 50 checks).

## Object layout produced (under each scratch dir)

```
blobs/<hex>            # one column stream, key = content hash
manifests/<hex>        # one part manifest, key = its own content hash
refs/<part_name>       # tiny ref: -> manifest_hash (tombstone refs have empty manifest_hash)
detached/<part_name>   # detached refs (reachability root)
frozen/<snap>/<name>/<file>   # FREEZE: materialized real bytes (independent of blobs/)
```

## Deliberate simplifications (vs. the real design / ClickHouse)

This PoC proves the **algorithms**, not production fidelity. It intentionally omits / simplifies:

- **Not integrated into ClickHouse.** No `IDisk`/`IObjectStorage`/`IMetadataStorage`/`DataPartStorage`; no real part reading (columns/marks/index), no SQL. A part here is just a `name → bytes` map.
- **Hash** is a deterministic FNV-1a-based 128-bit hash, not `cityHash128`; blob key omits the size guard the design recommends (we note it in the design, not here).
- **Single process, in-memory pins and lifecycle clock.** Keeper, replication `/log`, per-replica refs, and the real `grabOldParts`/`old_parts_lifetime`/`isSharedPtrUnique` are modeled by a single `Catalog` + a logical `now` clock. The "union over replicas" is a single ref set.
- **No real concurrency / GC coordinator election**, no S3 conditional writes, no MARK-epoch revalidation under a GC lock (the design's TOCTOU hardening is described there, not exercised here).
- **Covering rule** is a faithful but simplified `contains` (handles merge/mutation/range/tombstone); it does not model patch parts, `REPLACE_RANGE` multi-ref commits, or the `DROP_PART`-vs-concurrent-merge race (all discussed in the design's residual risks).
- **FREEZE** copies bytes into a separate namespace (demonstrating the "materialize, don't reference" fix) rather than into a real on-disk `shadow/`.
- Manifest does not yet split the inline set exactly as the design's §2 (it inlines all non-`.bin`/marks files); the boundary is a tunable, not a correctness property.

In short: this is a faithful **executable model** of the storage/GC core, useful to reason about
correctness and to seed a real implementation — not a drop-in ClickHouse engine.

## Verification

The model and tests were put through an adversarial code review (a subagent that compiled probe
programs against the build and diffed `PartInfo::contains` against the real
`MergeTreePartInfo::contains`). It found and we fixed **three correctness-fatal bugs**:

1. **Covering ignored mutation** — `contains` returned `true` on `level > o.level` alone, so a stale
   merge (level 1, mutation 0) wrongly covered a fresher mutated part (mutation 5) → silent data
   regression. Rewritten to the canonical CH predicate (`mutation >= o.mutation` is unconditional);
   test 15 now pins this.
2. **Tombstone mutation** — once (1) was fixed, a `mutation=0` tombstone would fail to cover a mutated
   part; tombstones now carry `mutation = INT64_MAX`.
3. **GC fail-open on a missing manifest** — a live ref pointing at an absent manifest object caused
   GC to drop the ref's blobs. `markReachable` now **fail-closes** (throws rather than widening the
   deletable set).

Plus minor robustness fixes: `attachPart` refuses to clobber a live ref; `Manifest::deserialize`
validates the magic and bounds-checks; partition ids are validated (no `_`/newline). The covering
rule now mirrors `MergeTreePartInfo::contains` including the mutation constraint.
