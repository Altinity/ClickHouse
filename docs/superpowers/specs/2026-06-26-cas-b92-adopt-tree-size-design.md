# B92 — carry `tree_size` on the adopt/relink wire — spec + plan

**Status:** design + plan (unattended ritual, 2026-06-26). Branch `cas-vfs-path-mapping`. Pre-release: no migration.

## Problem
`adoptTree` (whole-tree adoption: FREEZE / detached re-attach / replication relink) records its tree
dependency with **size 0**, which flows into `RefPayload.tree_size` at publish (`CasBuild.cpp` `publish`/
`precommit`: `payload.tree_size = … : dep_it->second.size`). `Resolved.tree_size` is reader-facing so the
M-W wiring can build `StoredObject`s without a HEAD; an adopt-published part therefore carries a wrong 0.

## Root cause (grounded)
`adoptTree` already HEADs the tree object via `observeAndAdmit(Tree, hash, key)` (`CasBuild.cpp:517`).
Inside `observeAndAdmit(kind, hash, key, hr)`, the size is computed as `logical_size = hr.size -
header_len` **for blobs**, but **for trees it is hard-coded to 0** (an over-conservative "trees would
need a decode" assumption). That 0 is the bug. No decode is needed: a tree object is laid out as
`[blob_header_len-byte envelope][encodeTree payload]` (2b/2c freeze — header padded to `blob_header_len`,
no payload trailer), so the **payload size = `hr.size - blob_header_len`**.

## The `tree_size` semantics (locked by this spec)
`tree_size` is the **tree PAYLOAD size** (`encodeTree(entries).size()`), NOT the full S3 object size.
Evidence: the built-tree path records `encoded.size()` (`stageTree:557`, `retained_trees[id]=encoded`),
and the `Subtree` `TreeEntry.file_size` convention is "tree payload size" (`CasTreeCodec.h:21`). The
future M-W `StoredObject` consumer adds the envelope itself. So adopt must record the **payload** size to
preserve the round-trip invariant: *a tree adopted-and-republished gets the same `tree_size` as if freshly
built*.

## Fix (one place, free — reuses the HEAD already done)
In `observeAndAdmit(kind, hash, key, hr)` compute the tree payload size the same way as blobs:
- for `kind == Tree`: `logical_size = hr.size - header_len` (with the **same** underflow guard the blob
  arm has: `hr.size < header_len → CORRUPTED_DATA`), where `header_len = store->poolMeta().blob_header_len`.
- Remove the tree-specific `logical_size = 0`.
This makes `adoptTree`'s dep carry the payload size → `publish`/`precommit` emit the correct
`RefPayload.tree_size` → relink propagation (`ContentAddressedTransaction.cpp:153,156`,
`tree_evidence.file_size`/`payload.tree_size = resolved->tree_size`) carries a correct value forward.
Update the `adoptTree` FOLLOW-UP(M-W) comment to state it's resolved.

**Why not a new HEAD or caller-supplied:** the HEAD already happens in `observeAndAdmit`; caller-supplied
from `Resolved.tree_size` would propagate a stale 0 and needs every caller to have it. The
`hr.size - header_len` derivation is authoritative and self-contained.

## Risk check
- Only consumers of a tree `DepEntry.size` are `publish`/`precommit` `tree_size` (the thing being fixed);
  the closure walk uses `retained_trees`, not `dep.size`. So no other path changes. (Implementer MUST
  confirm via grep before relying on this.)
- `adoptFromTree` child-subtree adoption (`adoptEvidence`) already uses `entry.file_size`; verify the
  change to `observeAndAdmit` does not double-count or conflict there.
- Pre-release: refs published with 0 before the fix are throwaway; no migration.

## Plan (TDD, one commit)

### Task 1: failing round-trip test, then the fix
**Files:** `src/Disks/tests/gtest_cas_build.cpp` (or the closest existing build/adopt test fixture — grep
for `adoptTree`/`tree_size` test coverage first); `Core/CasBuild.cpp` (`observeAndAdmit`).

- [ ] **Step 1 — grep** `tree_size`, `adoptTree`, `DepEntry.*size` consumers to confirm the risk-check
  (only `publish`/`precommit` consume tree `dep.size`). Note findings.
- [ ] **Step 2 — failing test:** build a tree with real entries (so `encodeTree` payload size `T > 0`),
  publish ref `A`; in a fresh build `adoptTree(id)` + `publish` as ref `B`; resolve `B` and assert
  `resolved.tree_size == T` (matches ref `A`'s `tree_size`) and `!= 0`. Run → FAILS with `tree_size == 0`.
- [ ] **Step 3 — fix:** in `observeAndAdmit`, compute `logical_size = hr.size - header_len` for trees
  (same underflow guard as blobs); drop the `logical_size = 0` tree special-case. Update the `adoptTree`
  FOLLOW-UP comment (mark B92 resolved; note the payload-size semantics + `hr.size - blob_header_len`).
- [ ] **Step 4 — run:** the new test passes; full `--gtest_filter='Cas*:Ca*'` sweep — only the baseline
  red `CaWiringOps.FreezeViaHardLinksIntoShadow`.
- [ ] **Step 5 — commit:** `CA B92: carry real tree_size on the adopt path (observeAndAdmit tree payload size)`.

## Verification (done)
Build clean (`-Werror`); the round-trip test asserts `tree_size` survives adopt-republish; sweep
baseline-only red.
