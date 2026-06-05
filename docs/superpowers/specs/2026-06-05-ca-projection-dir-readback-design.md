---
description: Design spec for retiring the CA-only registerCarriedForwardProjectionForCA mutation workaround by extending the B59 read-your-writes overlay from file granularity to directory granularity, so loadProjections' existsDirectory probe sees a carried-forward projection staged in the open whole-part transaction during finalize — one mechanism (read-your-writes) instead of two.
sidebar_label: 'CA projection dir read-back'
sidebar_position: 15
slug: /superpowers/specs/ca-projection-dir-readback
title: 'Content-Addressed MergeTree — Projection Directory Read-Back'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Projection Directory Read-Back {#ca-projection-dir-readback}

**Status:** design spec (authored unattended; decision-maker = the implementing model per the 2026-06-05 night mandate). **Date:** 2026-06-05. **Branch:** `cas-mergetree-poc`. **Backlog:** retires the B58 workaround; extends B59. **Predecessor reading:** `2026-06-04-cas-mergetree-projection-readback-design.md` (B59, the file-granularity overlay).

## 1. Goal and scope {#goal}

Retire the CA-only `MutationHelpers::registerCarriedForwardProjectionForCA` workaround (`MutateTask.cpp`) and its B63 `rows_count`/index back-fill by closing the gap they paper over with the **same read-your-writes mechanism B59 already established** — just at directory granularity.

**The gap (verified).** During a mutation that carries a projection forward, the projection's inner files are hardlinked into the new (parent) part's **still-open** whole-part `ContentAddressedTransaction` (each `<proj>.proj/<inner>` is recorded in the transaction's `recorded` staging map via `createHardLink`→`recordBlob`). At finalize (pre-commit), `IMergeTreeDataPart::loadProjections` (`IMergeTreeDataPart.cpp:1375`) probes each projection with `getDataPartStorage().existsDirectory(name + ".proj")`. That routes to `DataPartStorageOnDiskFull::existsDirectory` (`DataPartStorageOnDiskFull.cpp:63-66`), which — unlike its sibling `existsFile` (`:53-61`, which gained a B59 prelude that consults the held transaction) — goes **straight to committed metadata** and never consults the open transaction. So the staged projection is invisible until the next part reload; B58 worked around this by manually registering the projection in-memory from the source part.

**In scope:** add a directory-granularity in-flight check to `ContentAddressedTransaction`; have `DataPartStorageOnDiskFull::existsDirectory` consult the held transaction before the committed path (mirroring `existsFile` exactly); delete `registerCarriedForwardProjectionForCA` + its two call sites + its B63 back-fill, letting the normal `loadProjections` path register the carried-forward projection.
**Out of scope:** Scan-B-style GC changes; any non-CA path (the prelude is `transaction`-guarded and CA-only in effect); `listDirectory`/`getDirectoryIterator` staged enumeration (only the existence probe `loadProjections` uses is needed — YAGNI).

## 2. Approaches considered {#approaches}

- **A (chosen) — extend the read-your-writes overlay to `existsDirectory`.** Add `ContentAddressedTransaction::hasInFlightDirectory(path)` that returns true iff any staged file (in `recorded`/`recorded_mutable` for the path's `(table_uuid, part_name)`) lives under `"<p->file>/"`. Have `DataPartStorageOnDiskFull::existsDirectory` consult `transaction` before the disk call, byte-identical in shape to the `existsFile` prelude. Then `loadProjections` finds the staged projection the normal way and registers it itself; delete the workaround. **One mechanism instead of two**; the deleted manual-registration + B63 back-fill stop being a parallel source of truth.
- **B — keep the workaround, do nothing.** Rejected: the user flagged it as redundant-in-spirit; it is a second mechanism duplicating what read-your-writes should provide, and the B63 back-fill (re-copying `rows_count`/index/minmax) is exactly the kind of hand-mirroring that drifts.
- **C — full staged directory enumeration (`listDirectory`/iterator overlay).** Rejected (YAGNI): `loadProjections` only needs existence; a full staged-enumeration overlay is more surface than any current caller needs.

## 3. Design {#design}

### 3.1 The transaction directory check {#txn-check}
Add to `ContentAddressedTransaction` (declared next to the existing `tryGetInFlight*` trio, `.h:190-198`; implemented next to them, `.cpp:217-266`):
```
/// CA read-your-writes (directory granularity): true iff this open transaction has STAGED (hardlinked /
/// written, not yet committed) at least one file under the directory `path` for `path`'s part. Mirrors the
/// file-granularity tryGetInFlight* trio; used by DataPartStorageOnDiskFull::existsDirectory so a carried-
/// forward projection dir is visible to loadProjections during finalize. Bails (false) on a path that is not
/// a part-relative file/dir (empty `p->file`).
bool hasInFlightDirectory(const std::string & path) const;
```
Implementation: `parsePartFilePath(path)`; if `!p || p->file.empty()` → false. Look up the per-part staging for `(p->table_uuid, p->part_name)`; if none → false. Return true iff any key in `recorded` OR `recorded_mutable` equals `p->file + "/"`-prefix (i.e. `key.starts_with(p->file + "/")`). (A staged file `<proj>.proj/<inner>` → the directory `<proj>.proj` exists in-flight.)

### 3.2 The `existsDirectory` prelude {#prelude}
`DataPartStorageOnDiskFull::existsDirectory` gains the same guarded prelude as `existsFile`:
```
bool DataPartStorageOnDiskFull::existsDirectory(const std::string & name) const
{
    auto path = fs::path(root_path) / part_dir / name;
    /// CA read-your-writes: a part still being assembled by this transaction can have a staged-but-uncommitted
    /// directory (e.g. a carried-forward projection hardlinked into the open whole-part txn) that committed
    /// metadata cannot see yet. Mirrors existsFile (B59) at directory granularity.
    if (transaction && transaction->hasInFlightDirectory(path))
        return true;
    return volume->getDisk()->existsDirectory(path);
}
```
This is the only production read-path change. `transaction` is the held disk transaction (same object `existsFile` queries); `hasInFlightDirectory` is added to whatever interface `tryGetInFlightFileSize` is declared on (the implementer confirms the exact type and adds it alongside, so non-CA transactions answer `false`/are never consulted — the `transaction &&` guard + the empty-`p->file` bail keep the non-CA path byte-identical).

### 3.3 Delete the workaround {#delete}
- Remove `MutationHelpers::registerCarriedForwardProjectionForCA` (definition + declaration, `MutateTask.cpp:1209-1271`) and its B63 `rows_count`/index/minmax back-fill.
- Remove its two call sites (`MutateTask.cpp:2051`, `:2358`) — the hardlink loop now just hardlinks; `loadProjections` (which runs later in finalize) registers the projection via the now-overlay-aware `existsDirectory`.
- Confirm the projection still loads with correct `rows_count`/index: `loadProjections`→the projection part's own `loadColumnsChecksumsIndexes` reads those from the staged files (which the file-granularity B59 overlay already serves via `existsFile`/`getFileSize`/`readFile`), so the back-fill the workaround did by hand is now done by the normal load path from the staged bytes. This is the crux to verify in tests (§5).

## 4. Error handling {#errors}
`hasInFlightDirectory` is a pure in-memory map scan; no I/O, no throw. A path that doesn't parse → false (defensive, same as the file trio). The `transaction &&` guard means a storage with no open transaction (the overwhelmingly common read path) is unaffected.

## 5. Testing {#testing}
- **Existing CA mutation+projection gtests/stateless** must stay green with the workaround DELETED — that is the proof the overlay path is equivalent. Especially any test that mutates a part carrying a projection and immediately reads the projection (the B58/B63 regression tests).
- **gtest (overlay unit):** open a CA transaction, stage `<proj>.proj/<inner>` (hardlink/write through the txn), assert `hasInFlightDirectory(.../<proj>.proj)` is true while `existsDirectory` through the committed metadata is false; after commit, the committed `existsDirectory` is true. A negative: a non-staged dir → false.
- **CA stateless smoke:** `04292_content_addressed_mutations` (+ any projection-on-CA test) green; the carried-forward projection is queryable in the SAME session that mutated it (no reload), proving `loadProjections` registered it during finalize.
- **Non-CA regression:** a plain mutation-with-projection test unchanged (the prelude is `transaction`-guarded; the empty-`p->file` bail and the non-CA transaction returning false keep it byte-identical).
- Full `ContentAddressed*` gtest suite green.

## 6. Plan phasing {#phasing}
1. Add `hasInFlightDirectory` to `ContentAddressedTransaction` (+ the interface it shares with `tryGetInFlightFileSize`); unit-test it; build.
2. Add the `existsDirectory` prelude; build.
3. Delete `registerCarriedForwardProjectionForCA` + call sites + B63 back-fill; build; run the CA mutation+projection gtests/stateless — they must stay green (the equivalence proof). Re-add nothing; if a test fails, the overlay is incomplete — fix the overlay, not by restoring the workaround.
4. Full suite + non-CA regression + backlog (B58 retired; B59 extended to directory granularity) + push.

## 7. Risks {#risks}
- **The carried projection's `rows_count`/index must come from the staged files via the normal load path.** If `loadProjections`→`loadColumnsChecksumsIndexes` reads something through a path NOT covered by the B59 file overlay (e.g. a `listDirectory` enumeration of the projection's inner files rather than known names), the projection could load empty/short. Mitigation: the §5 stateless test (query the projection in the mutating session) is the gate; if it surfaces a `listDirectory` dependency, that is a real finding → either extend the overlay minimally to cover it or document precisely (do NOT restore the by-hand back-fill silently).
- **Transaction interface placement.** `hasInFlightDirectory` must live on the same type `DataPartStorageOnDiskFull::transaction` is, beside `tryGetInFlightFileSize`. The implementer confirms the type before adding (avoid a parallel/incompatible method).
