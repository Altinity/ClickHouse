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

## 4. The fix — one cursor, in the snap (single source of truth)

Make the fold cursor **intrinsic to the snap** and **delete `gc/state.folded_cursor` entirely**, so there is only ONE durable cursor and `INV-SNAP-CURSOR-COHERENT` holds *by construction* (you cannot have a cursor without the edges it represents — they are the same write-once bytes). The trim-gate then becomes sound, and the cursor-skip class is **unreachable**, not merely caught.

> Note: an earlier draft kept *both* a snap cursor and `gc/state.folded_cursor` and asserted they were equal. That is a half-measure — two separately-durable values that can still diverge, with a tripwire that only *catches* the diverged state. The real merge removes the second value.

### Design principle
The cursor lives **only** in the snap. `gc/state` keeps just `snap_generation` (the pointer to the committed snap). Everything that needs the cursor reads it from `snap[snap_generation]`. A snap generation folded to cursor `c` is a different object than one folded to `c' ≠ c` even with identical edges, so byte-equal generation adoption is automatically cursor-aware.

### Changes
1. **`Core/CasGcSnap.{h,cpp}` — the cursor is part of the snap.** Add a per-root-shard `folded_cursor` map to `GcSnap` and serialize it in `encodeSnapFields`/`decodeSnapFields`. (With `snap_shards==1`, the single snap shard 0 carries the cursor map over all root shards.) **No migration** — the feature was never on prod, so reset the codec version to **v1** of the cursor-carrying format with no back-compat; a pre-existing snap is treated as absent and rebuilt by a full fold from cursor 0. Byte-equal adoption (`putIfAbsent` + the `snap[g] == candidate` comparison in `fold`/`cascadeAndPersist`) then rejects a different-cursor generation (probe upward), so a leader can never adopt an edge-equal snap folded to a different cursor.
2. **`Core/CasGcFormats.{h,cpp}` + `Core/CasGc.cpp` — delete `gc/state.folded_cursor`.** Remove the field from `GcState` and its codec. Read the cursor from `snap[snap_generation].folded_cursor` everywhere it is used (`fold` seed, `recheck` window, `trim`). At commit (`cascadeAndPersist`), write the advanced cursor *into the persisted snap*; the `gc/state` CAS advances only `snap_generation`. **Idle-round behavior:** the cursor now advances only when a snap is persisted (rounds that actually folded/stripped/pruned). A truly idle round (no records, no strips) neither advances the cursor nor writes a snap; the next round just re-folds an empty window. This is conservative (a lagging cursor only ever causes *more* re-folding, never less — it cannot reopen the cascade-vs-recreate race) and adds **no** per-round snap writes.
3. **`Core/CasGc.cpp` — `trim` gates by the snap-resident cursor**, which is provably the committed snap's own fold extent (`:144`). By construction it can only drop records whose edges are in that snap — no over-trim.
4. **`Core/CasGc.cpp` — fail-closed snap↔journal coherence guard (the safety net).** At round start, before any retire/delete, for each live root shard verify every *latest-for-ref* journal `Add(ref→T)` with `at_version ≤ snap.folded_cursor[shard]` is reflected as a root edge in the snap (its presence is what "folded" means). On any violation raise `CORRUPTED_DATA` and delete nothing. This is the load-bearing no-loss net: even if some unforeseen path produced a cursor ahead of the snap's edges, the guard refuses to delete — a loud `CORRUPTED_DATA`, never a silent `dangling`. It is cheap (a bounded journal scan ≤ cursor) and, with change 1, never fires in normal operation.

### What is explicitly preserved (per the spec/code reconciliation)
- `retire → fence → recheck(fold-through-fence) → single exact-token delete` — the cross-node delete-race barrier. Untouched.
- `incarnation_tag` + exact-token `deleteExact` — ABA / `INV-NO-RETURN`. Untouched.
- per-server build watermark (`protectedByLiveBuild`), GC lease + `gc/hb` heartbeat, namespace registry + fence-time-universe. Untouched.
- in-degree as an idempotent **edge set** (not a signed counter). Untouched.
- No EBR resurrection (generations-in-key / tombstones / sessions) is reintroduced.

### Why this is producer-agnostic — and safe even if a residual path exists
We do not need to pin the exact cross-leader interleaving (it resisted prior pinning). With one cursor, the incoherent `(edges, cursor)` state is **structurally unreachable** (change 1+2). The snap↔journal coherence guard (change 4) is the independent net: should any unforeseen path still produce an incoherent state, the guard fires and refuses to delete. INV-NO-LOSS is thus guaranteed by the structure *and* the guard.

## 5. Compatibility / rollout
- **No migration.** Pre-GA on `cas-mergetree-poc`, never on prod. The snap codec is **reset to v1** of the cursor-carrying format and `gc/state.folded_cursor` is removed; any pre-existing `gc/snap` / `gc/state` from the old format is treated as absent and the GC rebuilds by a full fold from cursor 0 (the normal cold-start path), never trusting an old cursor for a trim or delete.

## 6. Tests (TDD)
- **FIRST deliverable — the failing gtest (DONE, committed):** `src/Disks/tests/gtest_cas_b140_dangle.cpp` rewritten to the faithful shared-blob cursor-skip shape — a blob `B` shared by a live `rb_live→T_live→B` and a distinct live `rb_cur→T_cur→B` (via `reuseBlob` = the soak's cross-node adopt); an injected under-counted snap (T_live expanded *with* its edge — no marker-without-edges — while `T_cur→B` is absent and the cursor is past both Adds); a real GC round drops+strips `T_live` and deletes `B` while `rb_cur` is live. **Confirmed RED in the real code** (`dangling=1, B_present=false`). It goes GREEN via **change 4** (the snap↔journal coherence guard catches the injected cursor-ahead-of-edges state at round start and refuses to delete). Note: an *injection* test validates the guard, not the structural prevention — the structural half is validated by the sibling adoption test and TLA+.
- **Sibling tests:** (i) **structural** — two `GcSnap`s with identical edges but different `folded_cursor` must encode to *different* bytes, so byte-equal adoption rejects (RED today: identical bytes; GREEN after change 1). This directly tests the cursor-in-snap prevention. (ii) `CasReuseGcRace.ReuseOfBlobDeletedBeforePublish` stays **green**. (iii) the B140-leak red (`CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`) stays red (separate, out of scope).
- **TLA+:** `CaB140DangleMerge.tla` is the oracle — `m_cursorskip` (live code shape) must dangle, `m_merged` must be clean. Keep both committed.

## 7. Soak validation
Rebuild `clickhouse`; quick soak (~15 min) → analyze (`dangling=0`, `forgotten_*` healthy, `CAGCDEL`/`CAREUSE` attribution); then a **12h soak with chaos**; verify `dangling` stays 0 across all `gc_checkpoint`s. Keep the instrumentation through validation.

## 8. Out of scope
- B164b (journal size: trim cadence / externalizing the journal as a rotated append-log). Separate backlog item; complementary, not required here.
- The clean-disk re-architecture (blob/tree/root → file/folder/mount). The existing architecture is sound; this is a coherence bug, not an architectural flaw.
- `snap_shards > 1` (forbidden today; unchanged).
