---
description: 'Closure record for R1 verbatim-file rebirth aliasing: namespace-life re-key evidence, lifecycle consequences, recreate-only cuts, and the loose-mountpoint audit.'
sidebar_label: 'R1 verbatim-file aliasing closure'
sidebar_position: 63
slug: /superpowers/cas/r1-verbatim-file-aliasing-closure
title: 'R1 verbatim-file aliasing closure'
doc_type: 'reference'
---

# R1 verbatim-file aliasing closure {#r1-verbatim-file-aliasing-closure}

## Disposition {#disposition}

R1 is closed. Namespace files are keyed by an opaque namespace life, so debris from one life cannot
be addressed through a later life of the same namespace name. The read and delayed-write contracts
retain the life selected before rebirth rather than resolving the name into its successor. The
remaining loose mountpoint-object surface has a separate, server-root-qualified identity and does
not carry a namespace-rebirth alias.

This note records the landed evidence required by Stage B Task 9. It does not reopen the design
alternatives that the namespace-life amendment already decided.

## Evidence by R1 sub-hazard {#evidence-by-r1-sub-hazard}

### Unqualified namespace-file keys {#unqualified-namespace-file-keys}

Task 4b commit `827bc0a9189` changed namespace-file APIs and keys from name-only identity to an exact
`NamespaceLifeId`. Its tests establish both sides of the new contract:

- `CasNsFileIncarnation.ColdReaderUsesCatalogCutWhileOldFileSurvivesRemoval` leaves life 1's object
  physically present and hidden from `LIST`, admits life 2 under the same namespace name, and proves
  the life-1 bytes remain unreachable from life 2.
- `CasNsFileIncarnation.RebirthDoesNotWaitForFilesToBeEmpty` leaves `_files` debris present while
  the old life reaches terminal fold evidence and proves that same-name rebirth does not depend on
  physical emptiness.

Task 4d then removed logical namespace text from the physical life prefix. Commit `6a3dd6a9245`
introduced the opaque split layout; review fixes `2b8475fc6f6` and `21ce9e99f4d` made the catalog
authority and its bootstrap fail closed. The final namespace-file key is
`cas/ns/state/<life_id>/_files/<relative-name>`, as recorded with the focused and full gate evidence
in `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task-4d-report.md`.

The key, rather than successful deletion, is the safety boundary: an omitted old-life object can
leak storage, but no later catalog row can make that opaque life id denote the reborn namespace.

### Read and delayed-write aliasing {#read-and-delayed-write-aliasing}

Fresh namespace-file resolution accepts a `Live` catalog life and rejects `Creating`, `Removing`,
and absent rows. `CasNsFileIncarnation.FreshReaderAssignsOnlyLiveCatalogLifeWithoutMutation` proves
that classification and zero durable mutation. Once a reader or `CaInlineWriteBuffer` has obtained a
life, it deliberately retains that exact `NamespaceLifeId`; it does not re-resolve a namespace name
during the operation.
`CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey` proves that
steady namespace-file operations add no hot-path catalog request.

Commits `4048163f0dd` and `3b952c6cbde` add the real disk-path contract suite
`CasNamespaceFileReadContract.*`:

- `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes` seeds different same-name bytes in two
  lives and proves a stale life-1 reader returns only life-1 bytes or absence, never life-2 bytes.
- `DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes` opens a real `CaInlineWriteBuffer` under
  life 1, makes life 2 live, then proves delayed finalization cannot change life 2's token or bytes
  and can land only under life 1 or fail with the typed stale-write exception.

The suite's controlled RED mutations re-resolved the catalog name on read and on buffer finalize;
both tests failed for the expected successor-alias behavior before the mutations were removed. The
report is `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task6-ns-file-contract-report.md`.
The obsolete `namespaceFilesReadable` TOCTOU gate and `namespaceIsRemoved` surface are absent from
the current tree.

### Physical-empty lifecycle proof {#physical-empty-lifecycle-proof}

Commit `827bc0a9189` removed `_files` from lifecycle-gating enumeration and deleted
`namespacePhysicallyEmpty`; `CasNsFileIncarnation.RebirthDoesNotWaitForFilesToBeEmpty` is the direct
regression proof. Task 5's generation-7 core (`bf396ffa50d`) retired the `_cleanup` terminal-marker
class and deleted the lifecycle-specific `runNamespaceCleanupPasses` physical pass. Commit
`d278d130024` removed the remaining synchronous checkpoint-deletion duty.

Physical debris is now outside the rebirth proof. The perpetual dead-life janitor added by
`111bb12a407` is the sole namespace-life debris owner; the orphan-manifest sweep separately owns
orphan manifest bytes. Both are cleanup mechanisms, not identity or rebirth gates. An incomplete
`LIST` can therefore defer reclamation and leak bytes, but cannot expose an old file through a new
life or prevent the new life from being admitted.

### Recreate-only migration cuts {#recreate-only-migration-cuts}

Task 4d commit `6a3dd6a9245` introduced the generation-6 opaque-life layout cut, followed by Task 5
commit `bf396ffa50d` and the generation-7 unified ref-life wire cut. Both are recreate-only format
boundaries: there is no compatibility parser, copy-forward path, or fallback that could reinterpret
an older name-keyed object as a current life-keyed namespace file. This satisfies Constraint 14.

## Loose mountpoint-object audit {#loose-mountpoint-object-audit}

The loose-object conclusion is negative: no R1 residue exists on this surface.

`ContentAddressedTransaction::writeFile` first sends a path recognized by `parseTableFilePath` through
`namespaceLife`, captures that exact life, and writes with `putNamespaceFile`. If the path is neither
a part file nor a table-level file, the fallback constructs `<server_root_id>/<path>` and uses
`putMountpointObject`. `ContentAddressedMetadataStorage::existsFile` and `getStorageObjects` preserve
the same classification on reads and probes.

`Layout::namespaceFileKey` maps the table-file branch to
`cas/ns/state/<life_id>/_files/<relative-name>`. `Layout::mountpointObjectKey` maps the loose branch
to `roots/<server_root_id>/<path>` with no namespace, catalog row, life id, or later name-resolution
step. A same-name namespace rebirth can change only the catalog-selected table-file life; it cannot
retarget a loose mountpoint key.

## Residual and backlog disposition {#residual-and-backlog-disposition}

No verbatim-file rebirth-aliasing residue survives. The namespace-file family is closed by the
opaque-life re-key and retained-life operation contracts, while loose mountpoint objects are outside
namespace identity by construction. No new register item, backlog entry, or follow-up specification
is required.
