---
description: 'Authoritative user directive (2026-07-29): Stage B amendments — NamespaceLifeId over ref objects AND namespace files, LIST-independent recovery, O(1) _ckpt, prepareRefChunk/chooseRecoveryGrounding extractions.'
sidebar_label: 'Stage B namespace-life amendments'
sidebar_position: 64
slug: /superpowers/specs/2026-07-29-cas-stage-b-namespace-life-amendments
title: 'Stage B amendments — namespace-life identity for refs and files'
doc_type: 'reference'
---

# Stage B amendments — namespace-life identity for refs and files {#stage-b-namespace-life-amendments}

Authoritative user directive, received 2026-07-29 late evening, recorded verbatim below (the
motivated final form; a terse draft preceded it and is superseded). Scope and intent are
authoritative; the Stage B plan supplies task ordering and test commands; previously rejected
alternatives are not reopened.

## The directive, verbatim {#directive}

Apply the following design amendments to the existing Stage B implementation plan. Treat this prompt as authoritative for scope and intent. Read only the current Stage B plan for task ordering, dependencies and test commands; do not reopen previously rejected alternatives.

Inspect current HEAD first because Stage B may already have started. Follow `AGENTS.md`: preserve unrelated changes, use new commits, and never amend or rebase.

### Context and objective {#context-and-objective}

Stage B introduces an authoritative namespace catalog and random namespace incarnations. The original plan scoped incarnation to ref objects only, leaving namespace files at:

`roots/<namespace>/_files/<name>`

That leaves a correctness hole: an old file omitted by LIST can survive namespace removal and become visible after the same logical namespace is created again.

We want to close that hole while Stage B is already replumbing namespace identity. However, namespace files—especially `MergeTreeDeduplicationLog` segments—are on a sensitive insert path. Therefore this change must fix object identity without redesigning file persistence or adding requests to that path.

### Required design changes {#required-design-changes}

#### 1. Generalize namespace identity {#generalize-namespace-identity}

Incarnation must qualify both ref objects and namespace files.

Use a general type such as:

`NamespaceLifeId { RootNamespace ns; UInt128 incarnation; }`

Replace the planned `RefNamespaceId`. The type must:

- reject zero incarnation;
- have no default construction;
- use canonical fixed-width lowercase hex;
- be created only from a catalog entry or an already-held live handle;
- not implicitly convert to `RootNamespace`.

Delete all namespace-only ref and namespace-file key overloads. This makes accidentally dropping the incarnation unrepresentable.

#### 2. Incarnation-key namespace files {#incarnation-key-namespace-files}

Store namespace files as direct objects under:

`roots/<namespace>/<incarnation>/_files/<relative-name>`

Keep these unchanged:

- manifests and their existing globally unique build identities;
- loose mountpoint objects outside namespace ownership;
- the current direct-object implementation of namespace files.

Once the catalog entry is removed, files from that incarnation become structurally unreachable. Namespace rebirth must not wait for `_files` to become physically empty.

LIST may discover old file debris for lazy cleanup, but LIST omission may only leak storage. It must never affect visibility, rebirth or deletion safety.

#### 3. Make recovery fully LIST-independent {#recovery-list-independent}

After the catalog lifecycle lands:

- `Creating` namespaces are never recovered or published;
- `Live` namespaces require a readable `_ckpt` with `life_epoch`;
- ordinary `Removing` namespaces also require `_ckpt`;
- the special `Removing` + missing `_ckpt` finalization window is handled by removal/decommission logic, not recovery;
- missing required `_ckpt` is corruption.

LIST may still:

- offer a newer snapshot candidate;
- provide additional diagnostic witnesses;
- nominate garbage for cleanup.

LIST must not determine genesis or committed history. Remove the fallback that starts from `hint_log_ids.front()`. Never fabricate `life_epoch` with `value_or`.

For the same exact objects, full, empty, partial and reordered LIST results must reconstruct the same logical state. Only request count, diagnostics and discovered garbage may differ.

#### 4. Strengthen `_ckpt` {#strengthen-ckpt}

`_ckpt` must remain a fixed-size product of scalar monotone facts. Its encoded size must be `O(1)` in the number of refs, files, transactions and writer epochs.

Do not add maps, collections or cardinality-growing fields. Such state belongs in a separate immutable object or ledger.

After `_ckpt` is incarnation-keyed:

- unknown `life_epoch` joined with `E` yields `E`;
- `E` joined with `E` yields `E`;
- two different present `life_epoch` values in one incarnation are corruption, not `max`;
- `checkpoint_snapshot_id` and `last_epoch_seal` continue to merge by semantic maximum.

Delay the conflicting-`life_epoch` behavior change until `_ckpt` has been re-keyed: before incarnation separation, different namespace lives could still share the old key.

### Implementation improvements {#implementation-improvements}

#### 1. Extract pure preparation from `commitRefChunk` {#extract-prepare-ref-chunk}

`commitRefChunk` currently mixes candidate construction, persistence and settlement. Extract a backend-free preparation function, approximately:

`prepareRefChunk(state, id, chain_link, ops, admitted_generation)`

It should prepare:

- candidate `RefTableState`;
- candidate base id;
- `RefLogTxn`;
- canonical key;
- sealed bytes;
- complete prebuilt wedge;
- optional birth `_ckpt` contribution.

Motivation: preparation is deterministic protocol work and can be tested exhaustively without a backend. It must remain entirely before the first durable effect.

Preserve:

- backend request counts;
- all fault and ambiguity semantics;
- the existing wedge protocol;
- allocation-free post-durable install regions;
- existing post-durable fault seams.

Do not broadly rewrite settlement in the same change.

#### 2. Extract recovery grounding {#extract-recovery-grounding}

Introduce a pure helper such as:

`chooseRecoveryGrounding(catalog_state, ckpt, greatest_hinted_snapshot)`

Rules:

- choose the greater checkpoint/hinted snapshot as the base;
- with a base, walk from its successor;
- without a base, walk from `{life_epoch, 1}`;
- never derive genesis from log LIST results;
- fail closed when the lifecycle requires information that is absent.

Audit every remaining use of recovery LIST data. If any LIST result still affects correctness rather than performance, diagnostics or leak-only cleanup, stop and document the unresolved dependency instead of preserving a fallback.

#### 3. Clarify snapshot publication {#clarify-snapshot-publication}

Rename or split `trySnapshotPublishOnce` so its two durable effects are explicit, for example:

`tryPublishSnapshotAndAdvanceCheckpointOnce`

Preserve the required ordering:

1. immutable snapshot body becomes durable;
2. `_ckpt` advances;
3. the new snapshot is adopted in memory.

Do not change its retry/backoff semantics.

### Namespace-file implementation requirements {#namespace-file-requirements}

- Every namespace-file API accepts `NamespaceLifeId`.
- Hot reads and writes use an already-held life handle; they must not issue a catalog GET.
- Delayed write-buffer callbacks capture the exact incarnation present at admission.
- A stale reader may return stale data or `NotFound`, but never data from a newer incarnation.
- A stale writer may only target its old incarnation and must never write into the new one.
- Remove `_files` from mandatory namespace-removal LIST deletion.
- Lazy cleanup uses exact-token deletion and the deposited incarnation.
- A resumed cleanup pass must never re-derive incarnation from the current catalog entry.
- Keep the existing format-bump-B recreate-only migration; reject legacy unqualified keys.

### Deduplication performance constraint {#dedup-performance-constraint}

Do not move namespace files into the ref log in this work.

`MergeTreeDeduplicationLog` rotates files frequently on the insert path because the CA disk does not support append. Adding catalog reads, blob uploads, ref-log appends or whole-directory manifest rewrites would directly affect insert latency.

Incarnation qualification must preserve the current namespace-file operation profile:

- no catalog request per file operation;
- no ref-log append;
- no blob upload;
- no folder-manifest rewrite;
- unchanged direct-object backend request counts.

### Required tests {#required-tests}

Add targeted tests for:

- an old file hidden by LIST across drop and rebirth;
- a stale reader after rebirth;
- a delayed writer finalized after rebirth;
- stale cleanup resuming after a new incarnation exists;
- zero catalog requests on namespace-file hot paths;
- unchanged request counts for rewrite, append, remove and dedup-log rotation;
- equal logical recovery under full, empty and partial LIST results;
- missing `_ckpt` for `Live`;
- conflicting `_ckpt.life_epoch`;
- rejection of legacy unqualified ref and `_files` keys;
- compile-time absence of namespace-only key overloads.

### Out of scope {#out-of-scope}

- Moving namespace files into the ref log.
- Folder-state manifests or a `_raw_files` object family.
- Changes to `MergeTreeDeduplicationLog`.
- Merging `Poisoned` into wedge.
- Refactoring `RefApplyState` in this series.
- Removing the entire plain-object component; loose mountpoint objects still require it.

`Poisoned` is not equivalent to wedge: it means a durable transaction may be missing from the cached view, so it must continue to block snapshot publication and trigger re-recovery. Record a follow-up to evaluate whether `ApplyPending` can later become debug-only, but do not make that change here.

### Execution {#execution}

First amend the Stage B plan with these changes. Then implement them in dependency order, keeping separate commits for:

1. plan amendment;
2. pure `commitRefChunk` preparation;
3. general namespace-life identity;
4. ref and file re-keying;
5. strengthened `_ckpt`;
6. recovery grounding;
7. cleanup/read-side closure;
8. snapshot naming or split.

Run the targeted tests and Stage B gates required by the plan and `AGENTS.md`.
