# B140-dangle fix v2: make the GC fold cursor coherent with the snap (cursor-in-snap)

**Status:** design, awaiting review · **Date:** 2026-06-18 · **Branch:** `cas-mergetree-poc`
**Supersedes:** `docs/superpowers/specs/2026-06-17-ca-b140-dangle-fix-design.md` (its mechanism and fix were invalid).
**Goal:** eliminate the data-loss bug where the content-addressed (CA) GC deletes a blob that a LIVE part still references (`fsck dangling`), while preserving `INV-NO-LOSS` / `INV-NO-RETURN` / `INV-OVER-COUNT-ONLY`.

## 1. Background — the GC state, in three durable pieces

The regular GC reclaims by folding the per-shard manifest journal into a durable in-degree snapshot, then deleting zero-in-degree objects behind a fence. Its durable state is **three separately-durable pieces**:

1. **snap EDGES** — `gc/snap/<generation>/<shard>` (write-once). Holds `edges` / `expanded` / `known`, from which in-degree is derived. The codec `GcSnap::encodeSnapFields` (`Core/CasGcSnap.cpp:264`) writes `snap_shard`, `generation`, `edges`, `expanded`, `known` — **it does NOT write the fold cursor.**
2. **the folded CURSOR** — `GcState.folded_cursor` (a per-shard map) in `gc/state` (`Core/CasGcFormats.cpp:89`). Records how far each shard's journal has been folded.
3. **the JOURNAL + trim** — the `journal` records embedded in each `roots/<ns>/<shard>` manifest, trimmed by `Gc::trim` (`Core/CasGc.cpp:118`) once folded.

The journal trim is **already gated** by the committed cursor (`Core/CasGc.cpp:144`, `INV-JOURNAL-COVERAGE`: only records with `at_version <= folded_cursor` are erased).

## 2. The bug (B140-dangle) — PINNED

**Symptom [CERTAIN]:** GC deleted a **dedup-shared** blob `B` referenced by two part-trees `T_live` (an older part) and `T_cur` (a newer, still-live part). The snap recorded only `T_live→B`; when `T_live` was stripped, `B` hit in-degree 0 and was deleted — while a live ref still resolves to `B` via `T_cur` → `fsck dangling` (data loss). Reproduced 3× in the 2-node soak; ground-truth-decoded on the real delete-generation snaps (gens 1280/1281).

**§6 resolved [CERTAIN, from `utils/ca-soak/logs/p9_instr_correlation.txt`]:** every dangling blob was `ch2 reuse @ round 334` → `ch1 delete @ round 340`, **deleted token == reused token**. Token equality proves a plain **`adopt`** of the existing, non-condemned incarnation (a `resurrect` would mint a fresh token). So: a cross-node writer dedup-adopted a present blob and committed a live part referencing it; the GC leader deleted that exact incarnation rounds later.

**The mechanism — CLASS pinned; exact interleaving cross-leader and unpinned.** The defect is that `folded_cursor` (piece 2) can become **incoherent with the actual fold extent of the committed snap edges** (piece 1) — running ahead of them. The already-present trim-gate (piece 3) then trusts `folded_cursor` and **trims journal records whose edges were never actually folded into the durable snap** — permanently losing the live ref's `T_cur→B` edge. The blob's snap in-degree is then a strict under-count; stripping the one counted parent drops it to 0; the exact-token delete succeeds (adopt never changed the token) → a live, reachable blob is deleted.

What is **CERTAIN (code fact):** the snap codec omits the fold cursor (`CasGcSnap.cpp:264`), so the snap edges and `folded_cursor` (`gc/state`) are **two separately-durable values with no enforced coherence between them**. What is **NOT statically pinned:** the exact cross-leader interleaving that makes them diverge. The single-leader commit is atomic and coherent by construction (the `gc/state` CAS advances `snap_generation` + `folded_cursor` together, `CasGc.cpp:448`, and the cascade persists the snap *with* the fence-window fold, `:400`); the divergence is **cross-leader** (consistent with the soak's B160 lease-churn correlation and the handoff's `[OPEN]` conclusion) and below static resolution. The TLA+ model below demonstrates that **whenever these pieces desync, the gated trim over-trims and the dangle results**, and that making them coherent closes it — so the fix does not depend on enumerating the exact interleaving.

This is a **snap-vs-truth divergence**: the GC's reachability view (`gc/snap`, what it deletes by) loses an edge that the authoritative manifest (`roots/`, what readers/`fsck` use) still has. The prior model could not catch it because it collapsed these three separately-durable pieces into one coherent structure.

### The broken invariant
> **INV-SNAP-CURSOR-COHERENT:** the committed `folded_cursor` of every shard equals the fold extent actually represented by the committed snap generation's edges. Equivalently: every journal record at or below `folded_cursor` has its edges present in `snap[snap_generation]`.

When this breaks, `trim` erases records whose edges are absent from the snap → permanent under-count → INV-NO-LOSS violation.

## 3. TLA+ evidence (the design oracle)

`docs/superpowers/models/CaB140DangleMerge.tla` models the three separately-durable pieces with two leaders, an in-memory work-in-progress fold discarded on lease loss, and a shared blob. Two independent producers, two fix flags (`TrimGated`, `CursorInSnap`). Exhaustive at `MaxLog=3, MaxGen=3` (`CaB140DangleMerge_RESULTS.md`):

| `TrimGated` | `CursorInSnap` | result |
|---|---|---|
| FALSE | FALSE | dangle |
| TRUE  | FALSE | **dangle** (cursor-skip — this is the live code's shape: trim already gated, cursor not in snap) |
| FALSE | TRUE  | dangle (trim-before-durable) |
| **TRUE** | **TRUE** | **clean, exhaustive, 5.33M states, non-vacuous** |

**Both halves are independently necessary and jointly sufficient.** The live code already has the trim-gate (`TrimGated=TRUE`); it lacks `CursorInSnap` (`FALSE`) — exactly the `dangle` row. The cursor-skip counterexample shows the gate is *unsound* while the cursor it trusts is decoupled from the committed edges.

## 4. The fix — cursor-in-snap (the merge)

Make the snap and its fold cursor **one coherent unit**, so `INV-SNAP-CURSOR-COHERENT` holds by construction; the existing trim-gate then becomes sound and the dangle is closed.

### Design principle
The fold cursor becomes **part of the snap's durable identity**. A snap generation folded to cursor `c` is a different object than one folded to `c' ≠ c`, even with identical edges. The committed `folded_cursor` is derived from (and asserted equal to) the committed snap's own recorded cursor — never advanced independently. A fail-closed assertion converts any residual incoherence into a loud `CORRUPTED_DATA`, never a silent delete.

### Changes (mirror the proven model)
1. **`Core/CasGcSnap.{h,cpp}` — store the cursor in the snap.** Extend `GcSnap` with a per-shard `folded_cursor` field and serialize it in `encodeSnapFields`/`decodeSnapFields` (with a codec version bump, `CAGS` v3→v4). Byte-equal generation adoption (`putIfAbsent` + the `snap[g] == candidate` comparison in `fold`/`cascadeAndPersist`) then becomes **cursor-aware** automatically: a generation folded to a different cursor is not byte-equal, so a leader cannot adopt it as its own — it probes upward and writes its own coherent `(edges, cursor)`.
2. **`Core/CasGc.cpp` — tie `gc/state.folded_cursor` to the committed snap.** At the commit point (`cascadeAndPersist`, the single `gc/state` CAS that advances `snap_generation` + `folded_cursor`, ~`:401`), set `folded_cursor` from the committed snap generation's own recorded cursor — not independently from fence versions. Add a fail-closed assertion at the persist/commit and delete boundaries: `snap[snap_generation].folded_cursor == gc/state.folded_cursor` (per shard); on mismatch raise `CORRUPTED_DATA` and delete nothing.
3. **`Core/CasGc.cpp` — `trim` unchanged in intent**, now sound: it keeps gating by `folded_cursor` (`:144`), which is provably the committed snap's fold extent.

### What is explicitly preserved (per the spec/code reconciliation)
- `retire → fence → recheck(fold-through-fence) → single exact-token delete` — the cross-node delete-race barrier. Untouched.
- `incarnation_tag` + exact-token `deleteExact` — ABA / `INV-NO-RETURN`. Untouched.
- per-server build watermark (`protectedByLiveBuild`), GC lease + `gc/hb` heartbeat, namespace registry + fence-time-universe. Untouched.
- in-degree as an idempotent **edge set** (not a signed counter). Untouched.
- No EBR resurrection (generations-in-key / tombstones / sessions) is reintroduced.

### Why this is producer-agnostic — and safe even if a residual path exists
We do not need to pin the exact interleaving by which `folded_cursor` outran the snap (it is cross-leader and resisted prior pinning). Cursor-in-snap makes `INV-SNAP-CURSOR-COHERENT` structural for **every** reachable durable state. Crucially, the **fail-closed coherence assertion** at the trim and delete boundaries (change 2) is the load-bearing safety net: even if some interleaving we did not foresee still produced an incoherent `(snap, folded_cursor)` pair, the assertion fires and **refuses to trim/delete** — converting a silent data-loss `dangling` into a loud `CORRUPTED_DATA` with no loss. INV-NO-LOSS is thus guaranteed by the assertion regardless of whether the structural coherence is complete; the structural change removes the need for it to fire in the common case.

## 5. Compatibility / rollout
- **Snap codec version bump** (`CAGS` v4). On decode of a v3 snap (no cursor), treat its `folded_cursor` as **unknown** and force a full re-fold from a safe floor (cursor 0) for that shard before any delete — fail-closed, never trust a v3 snap's implied cursor for a trim or delete. A clean restart re-derives a v4 snap.
- The feature is pre-GA on `cas-mergetree-poc`; no external on-disk compat obligation beyond the in-pool upgrade above.

## 6. Tests (TDD)
- **Rewrite `src/Disks/tests/gtest_cas_b140_dangle.cpp`** to the real shared-blob/cursor-skip shape: two `Gc` instances over one `Store` (the `gtest_cas_gc_round.cpp` template), a blob shared by `T_live` and a cross-node-published live `T_cur`, a path that advances `folded_cursor` ahead of the committed snap edges, then a trim + strip. Assert `runFsck().dangling == 0` after the fix; assert it reproduces (RED) before. Prefer a **natural** repro over byte-injection.
- **Sibling tests:** (i) byte-equal adoption across two leaders at different cursors must not adopt (must probe upward); (ii) a v3→v4 snap upgrade forces a re-fold and never trims/deletes on the implied cursor; (iii) `CasReuseGcRace.ReuseOfBlobDeletedBeforePublish` stays **green**; (iv) the B140-leak red (`CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`) stays red (separate, out of scope).
- **TLA+:** `CaB140DangleMerge.tla` is the oracle — `m_cursorskip` (live code shape) must dangle, `m_merged` must be clean. Keep both committed.

## 7. Soak validation
Rebuild `clickhouse`; quick soak (~15 min) → analyze (`dangling=0`, `forgotten_*` healthy, `CAGCDEL`/`CAREUSE` attribution); then a **12h soak with chaos**; verify `dangling` stays 0 across all `gc_checkpoint`s. Keep the instrumentation through validation.

## 8. Out of scope
- B164b (journal size: trim cadence / externalizing the journal as a rotated append-log). Separate backlog item; complementary, not required here.
- The clean-disk re-architecture (blob/tree/root → file/folder/mount). The existing architecture is sound; this is a coherence bug, not an architectural flaw.
- `snap_shards > 1` (forbidden today; unchanged).
