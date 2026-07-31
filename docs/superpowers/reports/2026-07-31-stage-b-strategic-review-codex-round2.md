---
description: 'Round 2 of the CAS Stage B strategic review — confrontation with the second reviewer and the catalog-state question'
sidebar_label: 'CAS Stage B strategic review round 2 — Codex'
sidebar_position: 101
slug: /superpowers/reports/2026-07-31-stage-b-strategic-review-codex-round2
title: 'CAS Stage B strategic review round 2 — Codex'
doc_type: 'reference'
---

# Round 2 conclusion

I accept the controller’s reading. There is no disagreement about mechanism:

- A temporary refusal only postpones rebirth. If the exact namespace can eventually return, a generation token is still required because omitted old namespace files could otherwise become visible in the new life.
- Permanent exact-`RootNamespace` non-reuse makes such debris inert without a generation.

Fable’s conclusion is therefore correct for eventual reuse. Its phrase “physical debris” is slightly too broad: incarnation qualifies ref objects and namespace files, but deliberately not part manifests or loose mountpoint objects (`Primitives/CasNamespaceLifeId.h:60-69`). The current design still separately discusses physical manifest emptiness (`Gc/CasGc.h:637-648`). Namespace-file debris alone is sufficient to establish Fable’s main point.

Fable’s stateless-CI claim is wrong as stated. Normal `Atomic` recreation creates a fresh table UUID and thus a fresh namespace (`ContentAddressedMetadataStorage.cpp:1232-1237`, `Pool/CasPool.h:505-508`). It does not wait on the prior namespace. A permanent `Retired` catalog would still grow by one historical row for each such UUID, but that is catalog-capacity consumption, not same-namespace refusal.

## 1. With incarnations gone, how namespace creation and removal would work

An incarnation-free state machine is logically coherent, but I no longer recommend shipping it.

The smallest persisted catalog state set is:

| State | Meaning |
|---|---|
| `Creating` | Namespace admitted under a creator fence; no publication permitted. |
| `Live` | Positive ownership and namespace-file writes permitted. |
| `Removing` | Positive writes forbidden; carries the removal identity/progress needed for recovery. |
| `Retired` | Terminal, permanently refuses exact-name reuse, excluded from the GC universe, never deleted. |

`Absent` is a lookup result, not a persisted state. `Decommissioning` should not be a namespace state: it is a server-root administrative mode which drives the affected namespaces through these same transitions. The catalog currently has the first three states (`Formats/CasRefCatalogFormat.h:22-36`).

Creation without incarnation would be:

1. Fenced catalog CAS from absent to `Creating {creator}`.
2. Conditional creation of the bare namespace’s `_ckpt`.
3. Fenced CAS from `Creating` to `Live`.
4. All ref keys and table-level verbatim files use the bare `RootNamespace`.

Removal would be:

1. Fenced CAS from `Live` to `Removing`; every positive publication path must reject this state.
2. Append the terminal `remove_namespace` record and record its exact `remove_txn_id` in the `Removing` row. A process stopping between these steps must be reconciled from the durable ref state, not by appending a guessed second terminal.
3. GC folds the terminal record, retires cleanup work, and publishes a seal with the namespace cursor pruned.
4. CAS `Removing(remove_txn_id)` to `Retired`.
5. Re-read and verify that exact `Retired` row, then exact-delete `_ckpt`. Failure merely leaves cleanup debris; the namespace remains refused.
6. Never delete the `Retired` row.

This places retirement directly in the catalog and requires no new retirement-marker object.

### Namespace kinds

The same state machine covers all real namespace kinds because `RootNamespace` is intentionally opaque; the core does not have table/shadow kinds (`Primitives/CasTypes.h:42-55`).

- Normal table refs and table-level plain files share one namespace. The latter currently resolve the table’s life before writing (`ContentAddressedTransaction.cpp:809-828`). A table containing only verbatim files is therefore born by its first such write.
- Loose mountpoint files are not namespaces. They remain path-owned plain objects with conditional overwrite/exact deletion and are drained by server-root decommission (`Pool/CasPool.h:599-611`, `Tools/CasDecommission.cpp:194-204`).
- Directory creation has no durable representation, so it does not create a catalog row (`ContentAddressedTransaction.cpp:983-994`).
- Detached and moving parts are ref-name prefixes inside the table namespace, not separate namespaces (`Parts/PartPathParser.h:43-56`).
- A shadow backup is a normal catalog namespace whose name is the literal shadow table directory (`ContentAddressedMetadataStorage.cpp:1246-1252`). First publication creates it; whole-shadow removal drives it to `Retired`.
- Decommission should enumerate catalog rows under the victim `server_root_id`: `Live` rows go through removal, `Removing` rows resume, `Retired` rows are already done, and a `Creating` row may be retired only after its creator fence is terminal. Because publication is forbidden while `Creating`, only a possibly published `_ckpt` needs exact cleanup. Loose mountpoint and staging objects remain separate drains. The current decommission already distinguishes namespace removal from those drains (`Tools/CasDecommission.cpp:131-173`, `:194-204`).

### Does a never-deleted `Retired` row break anything?

Yes: it creates a deterministic namespace-admission exhaustion point.

The catalog is a single raw token-CAS object with a 256 MiB cap (`Formats/CasFormat.cpp:141-154`). Admission checks both encoded catalog bytes and a per-entry fold-seal reservation (`Formats/CasRefCatalogFormat.cpp:256-263`, `:307-328`). Under current accounting, every historical row consumes both predicates; even after changing the fold reservation to count only active rows, the raw catalog cap remains.

This is not immediately fatal to safety. Existing namespace writes and removal can continue because removal bypasses admission. It is fatal to long-lived lifecycle availability: eventually every new namespace admission returns `LIMIT_EXCEEDED`. Its GET, decode, encode and whole-object CAS cost also grows with total historical births rather than current live cardinality.

Normal `Atomic` UUID creation does not bound this. It makes reuse rare, but every new UUID becomes another permanently retained historical row. Stateless churn therefore grows the catalog even though it does not receive a same-name refusal.

A smaller `Retired` representation changes only the constant:

- Storing a fixed digest rather than the full namespace is safe if collisions conservatively refuse both names, but remains unbounded.
- A fixed Bloom filter eventually saturates and refuses every new name.
- Sharding spreads write contention but does not compact the historical information.
- Moving retired history into immutable side objects recreates the additional object/history class the commissioner rejected.
- Rows below a permanently tombstoned server root could be discarded only if reusing that `server_root_id` is also permanently forbidden. The current owner protocol explicitly permits deliberate manual revival (`Pool/CasServerRoot.cpp:74-84`), so even this limited compaction requires a policy change. It also does nothing for live roots or pool-global shadow names.

There is no bounded exact compaction for arbitrary opaque names. Exact permanent non-reuse requires retaining information proportional to historical namespace count somewhere.

### Does the catalog-loss ordering survive?

Not in the current implementation.

`CasRefCatalog::read` treats an absent catalog as a virgin empty catalog (`Pool/CasRefCatalog.cpp:19-24`). If `Retired` is the only tombstone, catalog loss after `_ckpt` deletion turns the name back into an apparent first birth. My round-1 ordering depended on the retirement authority surviving outside the catalog.

The replacement would be a pool-level fail-closed catalog-existence invariant:

1. Bootstrap a new pool through an existing `_pool_meta` state such as `catalog_initializing`.
2. Create the empty catalog.
3. CAS `_pool_meta` to `catalog_ready`.
4. Once ready, absent or identity-mismatched `cas/ref_catalog` is corruption and all creation/removal/GC stops. It must never bootstrap as empty again.
5. Publish `Retired` before deleting `_ckpt`, and revalidate the terminal row before that delete.

That restores the normal-loss argument without a new marker class: after catalog loss, nothing can be born. But it adds another pool bootstrap protocol, does not address catalog rollback to an older version, and does not solve unbounded retired history. It is too much new lifecycle design merely to remove an already-landed incarnation.

My answer is therefore: retirement can live in the catalog semantically, but a permanent never-deleted `Retired` set is not an honest bounded production design.

## 2. Is forbidding `Ordinary` with CAS sufficient?

No.

It closes one obvious path: non-Atomic tables use stable `data/<db>/<table>` identities (`Parts/PartPathParser.cpp:294-301`, `:376-385`). `Ordinary` remains deprecated but registered (`Databases/DatabaseOrdinary.cpp:776-807`). A CAS-specific prohibition would also need to cover table storage policies, moves between database engines, and internal recovery—not merely reject `CREATE DATABASE ... ENGINE = Ordinary`. Atomic-to-Ordinary moves are currently allowed (`Databases/DatabaseAtomic.cpp:244-255`), and replicated-database recovery deliberately creates an `Ordinary` database so that it can discard a table UUID and recreate that UUID (`Databases/DatabaseReplicated.cpp:1600-1604`).

Even a complete prohibition leaves other exact-namespace reuse routes:

- `ATTACH TABLE ... UUID '<same>'` and explicit-UUID `CREATE TABLE` are supported. The in-memory UUID mapping is deleted after final table cleanup (`Interpreters/DatabaseCatalog.cpp:1630-1671`), after which the same explicit UUID can be locked again; collision protection is not permanent (`Interpreters/DatabaseCatalog.cpp:916-973`).
- Replicated database DDL can accept explicit UUIDs depending on `database_replicated_allow_explicit_uuid`, while internal replica DDL requires a UUID (`Interpreters/InterpreterCreateQuery.cpp:1445-1475`). Replica reconstruction using the replicated metadata’s UUID is therefore a real supported path.
- Stock `RESTORE` currently does not preserve the backup’s original table UUID: it explicitly generates a fresh coordinated UUID (`Backups/RestorerFromBackup.cpp:735-737`, `Backups/RestoreCoordinationLocal.cpp:59-87`). Thus the ordinary built-in restore path is not presently an alias. Any restore/attach mode that intentionally preserves the original UUID is equivalent to the explicit-UUID case and would be refused by permanent non-reuse.
- Shadow namespaces are unambiguously reusable. `FREEZE ... WITH NAME` derives the literal directory from the supplied name (`Storages/MergeTree/MergeTreeData.cpp:9914-9915`), and `UNFREEZE` removes that directory (`Storages/Freeze.cpp:176-180`). Reusing the name later reaches the same `RootNamespace`.

### Does shadow alone require a generation?

There are two coherent policies:

- Permanently retire the literal shadow namespace. A second `FREEZE ... WITH NAME '<same>'` fails, and the operator chooses another name. Then no generation is required.
- Preserve same-name shadow reuse. Then shadow needs a generation token, whether called `incarnation`, `backup generation`, or a mapping from the literal directory to an internal generated namespace.

It can technically be confined to shadow, but that is not a simplification. The core currently treats namespaces as opaque. A shadow-only identity creates two key grammars, two handle forms, conditional parsing throughout GC/fsck/decommission, and special cache and cleanup rules. Explicit UUID reuse also means shadow is not the only path unless all those workflows are forbidden. A uniform incarnation is smaller and safer than a special shadow incarnation plus a growing list of prohibitions.

### Exact operator refusal

Under the hypothetical permanent policy, the error should be terminal and actionable:

> CAS namespace `<root namespace>` is permanently retired; exact `RootNamespace` reuse is unsupported. Use a fresh table UUID, a different `FREEZE WITH NAME`, or a fresh `server_root_id`.

That is recoverable without object-store surgery when the identity is user-selected. It is not operationally equivalent for replicated recovery that must retain a coordinated UUID; changing the UUID can require rebuilding replicated metadata or coordination state. That supported workflow is one reason permanent refusal is too broad.

Under the recommended incarnation design, the temporary error is instead:

> CAS namespace `<root namespace>` is `Removing` for removal `<id>`; recreation is blocked until catalog cleanup and cursor retirement complete. Retry later.

After catalog entry deletion, the retry automatically receives a fresh incarnation. No object-store surgery or alternate logical name is required.

## 3. Restated recommendation

My recommendation changes plainly:

**Option 2 remains correct, but keep the incarnation. Withdraw my separate retirement-marker proposal and do not replace it with a permanent `Retired` catalog history.**

What changed my mind was not Fable’s temporary-refusal argument by itself; I already agreed with that mechanism. The change was tracing whether my stronger permanent-nonreuse policy could be implemented under the commissioner’s no-new-marker constraint:

- An exact catalog-resident tombstone set grows with every historical table UUID and eventually blocks admission at a hard capacity boundary.
- The current absent-catalog behavior invalidates my catalog-loss ordering unless another pool bootstrap invariant is added.
- Shadow-name reuse is explicit and ordinary.
- Explicit UUID and replicated recovery paths make permanent refusal a supported-workflow regression, not merely a prohibition on deprecated `Ordinary`.

For the pre-release week:

1. Keep the landed catalog and incarnation-qualified ref/namespace-file keys.
2. Concentrate fenced lifecycle mutations in `CasRefCatalog`; callers should not interpret raw catalog rows.
3. Give GC, fsck and decommission one immutable plan from one catalog snapshot.
4. Implement removal around `Live -> Removing`, terminal fold, cleanup/cursor retirement, exact `_ckpt` deletion, and catalog-entry deletion last. Only then may a same-name birth mint a fresh incarnation.
5. Do not add another stable retirement-marker object or a permanent `Retired` history. Keep any existing per-removal cleanup evidence finite and scoped to the removing incarnation.
6. Correct the Task 5 cost statement: normal `Atomic` stateless recreation does not wait on the old namespace; the important retry case is exact UUID or shadow-name reuse.

That preserves the catalog’s atomic-completeness value, retains the generation mechanism needed for supported exact-name reuse, and avoids introducing an unbounded terminal catalog in the final pre-release week.
