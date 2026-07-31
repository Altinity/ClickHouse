---
description: 'Independent strategic review of whether CAS Stage B should continue in its current form'
sidebar_label: 'CAS Stage B strategic review — Codex'
sidebar_position: 100
slug: /superpowers/reports/2026-07-31-stage-b-strategic-review-codex
title: 'CAS Stage B strategic review — Codex'
doc_type: 'reference'
---

# CAS Stage B strategic review — Codex {#cas-stage-b-strategic-review-codex}

## Verdict {#verdict}

**Option 2: revise Stage B now—keep the authoritative catalog but prohibit reuse of an exact "'`RootNamespace` and remove incarnation plumbing, because a single catalog `GET` supplies the point-in-time universe that paginated `LIST` cannot, while incarnation buys only rare exact-namespace rebirth and is the source of most of the plan'"'s spreading complexity.**

The "'`LIST` defect is real at the abstraction GC consumes. The captured rows show that one enumeration omitted two keys which were still present and which a later enumeration returned; the uploads preceded the returned successor key, and upload order was measured rather than assumed (`docs/superpowers/reports/2026-07-26-list-incompleteness-proof/README.md:24`, `docs/superpowers/reports/2026-07-26-list-incompleteness-proof/gc_anomaly_rows.txt:17`, `docs/superpowers/reports/2026-07-26-list-incompleteness-proof/blob_storage_log_three_keys.txt:2`). The proof does **not** identify the store-side mechanism and does not establish the behaviour on AWS S3 (`docs/superpowers/reports/2026-07-26-list-incompleteness-proof/README.md:54`). It therefore supports “the multi-page enumeration can omit durable objects”, not a claim about which individual page or service violated which guarantee.

The commissioner'"'s narrower reading does not make the catalog redundant. "'`forEachListedKey` stitches a walk from separately fetched pages (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h:369`), and the production backend constructs a fresh iterator with `start_after` for each page (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:1138`). Such a page set need not correspond to the namespace set at any instant. In contrast, `CasRefCatalog::read` obtains and decodes one object version and token (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:19`). Both observations become stale after they are taken, but only the catalog observation defines a cut: creation before the cut is in the body, while creation after it belongs to the already-required temporal argument. That argument protects late ownership through delayed condemnation, rematerialization/ordering, and the delete-site in-degree re-read (`docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:138`). “Both have TOCTOU” is therefore true but not dispositive; staleness after a coherent cut and a scan which may never have represented a cut are different proof obligations.

A bounded re-`LIST` or “retry when an out-of-order key is observed” is not a replacement: two finite scans can agree while both omit an entire namespace, and a whole-namespace omission supplies no observation that triggers the retry. A generation counter can work only if every creator and remover brackets publication, GC accepts only a stable generation, and an abandoned in-progress generation is recoverable. That is a lifecycle catalog with less information but essentially the same `Creating`/fence reconciliation problem. Since the catalog machinery is already landed, replacing it with that protocol before release would add risk without removing the hard part.

## What to keep from what is landed, and what to revert {#what-to-keep-and-what-to-revert}

Keep:

- All Stage A arithmetic-stream, seal, `_ckpt`, durable-hold, and destructive-frontier work. It fixes the observed within-namespace log omission by exact-key arithmetic rather than by trusting listed membership; current intake explicitly uses the listing only for a frozen work bound and computes every interior key (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1849`).

- Task 1b'"'s pure preparation extraction, commit "'`8d536022eab`. It removes decision logic from the pre-durable half of `CasRefLedger::commitRefChunk` without depending on catalog authority or rebirth.

- Task 2'"'s canonical catalog codec, bounded token-CAS loop, and admission accounting from "'`87a7fd7c618` and `5150a1cba16`, but amend `CatalogEntry` to remove `incarnation`. The single-object catalog is the valuable part. Its current retry loop already centralizes fresh-read/reapply semantics (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:64`).

- Task 3'"'s "'`Creating`/`Live`/`Removing` lifecycle states, creator-fence reconciliation, and `_ckpt`-before-`Live` ordering from `5a98ef5c4ae` and `3e1b6358104`. Keep these as catalog-owned birth mechanics, not as responsibilities reimplemented by `CasRefLedger`. The current implementation mints the incarnation at exactly one site (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:218`) and reconciliation changes only the creator fence, not that incarnation (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:245`), confirming that incarnation represents rebirth only, not lease change.

- Task 4-C'"'s catalog-authoritative universe and its fail-closed fixes: one catalog snapshot per GC round, no vacuous damaged-catalog proof, and reuse of that snapshot by destructive consumers. Current code already records why re-reading the catalog during cleanup can apply a plan to a different answer ("'`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h:416`). Keep the fsck correction which puts catalog namespaces into its universe as well.

- Lower-case Task 4c'"'s constant-size "'`_ckpt` invariant and the later “reject a decreasing `life_epoch`” correction, commits `5c5854ea054`, `f271e42744d`, `59cbe85640d`, and `0d9f08abe79`. Those are stream-integrity improvements independent of rebirth.

- The non-minting read/removal correction `9318862bd5d`. A read or `IF EXISTS` removal must not grow the catalog; `namespaceFilesLifeIfReadable` now expresses absence without a write (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:4125`).

Revert or replace, surgically rather than by blindly reverting mixed commits:

- Remove Task 1/1c'"'s "'`NamespaceLifeId` identity and incarnation segment from ref and namespace-file keys (`9f9514a90f1`, `e0d333890d6`, `147875b6748`). The type exists solely to make same-name lives disjoint (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasNamespaceLifeId.h:60`). Under the proposed no-reuse rule, `RootNamespace` is the complete identity again. Retain unrelated parser strictness and enumeration exception fixes from those rounds.

- Remove lower-case Task 4b'"'s “rebirth waits for nothing” keying ("'`827bc0a9189`). Namespace files may return to `roots/<ns>/_files/...`; old files cannot alias a new life because that exact namespace can never have a new life.

- Replace Task 4-C'"'s incarnation-specific implementation: "'`live_incarnation`, the foreign-incarnation pre-filter, the current-versus-stale incarnation contradiction detector, all `resolveLifeOrSentinel` uses, and the Stage-A sentinel. The catalog-backed namespace **set** remains authoritative. The current filter and its follow-on anomaly machinery occupy `CasGc.cpp:1372-1490`; they are consequences of overlapping lives, not of catalog authority itself.

- Do not mechanically roll format generation 5 back. Task 4-A commit `21617aedda2` contains unrelated fixes and the branch is pre-release/recreate-only. Define one final release grammar at the existing new generation (or bump once more if required), remove the incarnation path component, and regenerate the format goldens once.

- Replace Task 5 rather than executing it. Its present brief has accumulated cursor incarnation, deposited incarnation, warm-cache life invalidation, foreign-life janitoring, same-epoch rebirth, uncataloged-removal discrimination, and multi-removal reservation cases (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1196`, `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1226`, `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1244`). Those are not independent safety necessities; they are the downstream cost of allowing the same `RootNamespace` to become a second life before all evidence of the first has retired.

## The concentration proposal {#the-concentration-proposal}

Make `CasRefCatalog` the only owner of namespace lifecycle decisions, and give GC one small immutable `GcNamespacePlan` built from one catalog snapshot.

`CasRefCatalog::Snapshot` should expose typed operations rather than its raw `entries` vector:

- `lookup(ns) -> Absent | Creating | Live | Removing`;

- `liveAndRemoving() -> vector<RootNamespace>`;

- `ensureLive(ns, creator_fence, admitted_generation)`;

- `beginRemoval(ns, owner_fence)`;

- `isRetired(ns)`, backed by an exact, stable per-namespace retirement marker.

Move all of `CasRefLedger::resolveNamespaceLife` into `CasRefCatalog::ensureLive`; today that roughly hundred-line method separately reads entries, creates, resumes its own creator, reconciles a foreign creator, and maps outcomes (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:920`). Delete `resolveLifeOrSentinel`, `stageATransition`, and `fromCatalogEntry`. Readers call `lookup` and receive absence; writers call `ensureLive`; removers call `beginRemoval`. No caller outside `CasRefCatalog` interprets a raw lifecycle row.

At the GC boundary, `makeGcNamespacePlan(catalog_snapshot, listed_keys, parent_seal)` should return:

- the namespaces to walk;

- retired debris which is cleanup-only;

- catalog inconsistencies which suppress destruction;

- cursor keys to carry or prune.

`Gc::fold`, ref cleanup, namespace cleanup, fsck, and decommission receive that immutable plan or the same immutable catalog snapshot; none performs an independent catalog read. This finishes the direction started by `FoldResult::live_incarnation` without preserving its incarnation-specific representation. It also removes the current repeated resolution in fsck and decommission (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:711`, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp:150`). The present five mechanisms are visibly spread through the writer, GC, orphan sweep, protocol, fsck, and decommission; the backlog'"'s inventory found about 80 references and the current tree still exposes all five concepts ("'`docs/superpowers/cas/BACKLOG.md:3134`).

The replacement removal sequence is:

1. catalog `Live -> Removing`;
2. the fenced owner appends the terminal record;
3. GC folds the terminal record and performs one bounded cleanup pass;
4. GC writes a stable retirement marker derived only from `RootNamespace`;
5. GC durably publishes a fold seal with that namespace'"'s cursor pruned;
6. GC exact-deletes "'`_ckpt`;
7. catalog entry deletion is last.

Creation checks the retirement marker and the namespace `_ckpt` by exact key before admission and refuses if either exists. Ordering the retirement marker before `_ckpt` deletion means even catalog loss cannot turn the removal window into a fresh birth. Listed objects found after entry deletion are safe to treat as retired cleanup debris after an exact marker check. A listing omission can leak old objects, but it cannot make them visible again because the namespace is never admitted again. This preserves the fail-close direction without an incarnation, a foreign-life janitor, or a cleanup job capable of targeting a future life.

The write-hotspot evidence is real: 137 of 250 timeout lines named `cas/ref_catalog`, and ordinary stateless tests failed on emitted transient errors (`docs/superpowers/cas/BACKLOG.md:3265`). It is not a reason to discard the coherent cut, especially given the commissioner'"'s stated workload judgment. It is a release gate: rerun the S3 lane after read/removal non-minting and the simplified lifecycle, and do not enable destructive GC if ordinary creation still times out at that rate.

## Invariants to cut {#invariants-to-cut}

- **Cut fresh incarnation per rebirth and incarnation-qualified keys.** It currently buys structural non-aliasing when the exact same "'`RootNamespace` is removed and recreated while old objects or stale cleanup survive (`CasNamespaceLifeId.h:60`). Without it, exact namespace reuse must fail permanently and clearly. Normal `Atomic` drop-and-create is unaffected because the namespace contains the table UUID and a recreated table gets a fresh UUID (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:1232`, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h:505`). Reusing an old UUID, or a shadow namespace path, breaks: the operator must choose a fresh namespace.

- **Cut “rebirth waits for no physical cleanup” and foreign-incarnation janitoring.** It currently buys immediate same-namespace reuse even if `LIST` hides old `_files` or ref debris (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1018`). Without it, the retirement marker refuses reuse and leftover objects are leak-only. One stable marker per retired namespace is unbounded in historical namespace count, but it replaces the existing permanent cleanup-marker class rather than putting historical rows in the hot catalog; the active catalog remains `O(Creating + Live + Removing)`.

- **Cut incarnation-scoped cursors, deposited-incarnation cleanup, cached-life invalidation, and the current/dead-incarnation contradiction detector.** They currently buy safety when old and new lives overlap. Without overlap, the old cursor is pruned before the entry disappears, cleanup has no possible future target, and no cache can alias a new life. What breaks if the ordering is violated is severe: entry deletion before the cursor-pruning seal would reintroduce a permanent uncataloged cursor and pool-wide suppression, exactly the current Task 5 warning (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1250`). Therefore the ordering remains an executable invariant.

- **Cut admission reservation for repeated removal/rebirth rows.** It currently buys bounded serialization even when one namespace has multiple cleanup rows while no current catalog entry exists (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1286`). Under no reuse, there is at most one removal lifecycle for a namespace. Reserve for one active catalog row, one cleanup item, and its cursor/hold; any second removal is an exception, not another budget case.

- **Do not cut catalog authority, `Creating` publication exclusion, the `Removing` positive-ownership exclusion, the exact-next frontier probe, durable holds, or the delete-site in-degree re-read.** These prevent data loss independently of rebirth. In particular, a merely temporary “refuse while old objects survive, retry after GC” rule does **not** make incarnation unnecessary: with the observed enumeration defect, `LIST` cannot certify that every old object is gone, and a stale writer can be excluded only by the full fence/terminal argument. Incarnation becomes unnecessary only with permanent exact-`RootNamespace` non-reuse (or with a durable generation token, which is incarnation under another name).

## Cost {#cost}

This is four implementation tasks plus one release gate, not the remaining nineteen tasks of the current plan:

1. state the no-reuse contract, add the exact retirement marker and its failure/recovery tests, and settle the final recreate-only wire grammar;
2. remove `NamespaceLifeId` and incarnation path plumbing while preserving Task 1b and the independent parser/`_ckpt` fixes;
3. concentrate creation/read/removal decisions in `CasRefCatalog`, and build/pass one immutable `GcNamespacePlan`;
4. land the shortened removal sequence, cursor pruning, fsck/decommission use of the shared snapshot, and the destructive-gate tests;
5. run the full CA gates, both object-storage lanes, and a destructive soak.

The work is broad because Tasks 1/1c/4b deliberately made the identity compiler-visible, but it should be net code deletion and removes most of Task 5 plus the incarnation portions of Tasks 5b, 6, 7, and 9. It is worth one focused pre-release week because it trades a rarely used feature for a much smaller proof surface. If the deadline cannot absorb those four code tasks and the gate, the only defensible fallback is to ship with `UniversePolicy` still suppressing destructive GC; hurriedly landing current Task 5 and flipping Task 7b is not an acceptable deadline trade.

## What would change my mind {#what-would-change-my-mind}

I would keep the current incarnation design if a supported, pre-release-critical workflow must reuse the exact same `RootNamespace`—not merely the same SQL table name—and cannot instead mint a fresh table UUID or shadow namespace. That observation would make permanent retirement an unacceptable semantic regression, and incarnation would again buy something necessary.

I would drop the authoritative catalog as universe only after a repeatable, supported-store result and protocol argument establish that a quiescent full paginated `LIST` is complete, and that namespace creation/removal cannot move behind its cursor without a generation bracket which GC validates. The current proof'"'s inability to identify the RustFS mechanism is not that result; at the GC enumeration boundary it observed an omission.

I would replace the single catalog object with a sharded or head-CAS construction if production-rate namespace churn, after non-minting reads and the simplified lifecycle, still produces catalog contention comparable to the 2026-07-31 S3 lane. That would change the catalog representation, not the need for a coherent namespace cut.
REPORT" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
---
description: 'Independent strategic review of whether CAS Stage B should continue in its current form'
sidebar_label: 'CAS Stage B strategic review — Codex'
sidebar_position: 100
slug: /superpowers/reports/2026-07-31-stage-b-strategic-review-codex
title: 'CAS Stage B strategic review — Codex'
doc_type: 'reference'
---

# CAS Stage B strategic review — Codex {#cas-stage-b-strategic-review-codex}

## Verdict {#verdict}

**Option 2: revise Stage B now—keep the authoritative catalog but prohibit reuse of an exact `RootNamespace` and remove incarnation plumbing, because a single catalog `GET` supplies the point-in-time universe that paginated `LIST` cannot, while incarnation buys only rare exact-namespace rebirth and is the source of most of the plan's spreading complexity.**

The `LIST` defect is real at the abstraction GC consumes. The captured rows show that one enumeration omitted two keys which were still present and which a later enumeration returned; the uploads preceded the returned successor key, and upload order was measured rather than assumed (`docs/superpowers/reports/2026-07-26-list-incompleteness-proof/README.md:24`, `docs/superpowers/reports/2026-07-26-list-incompleteness-proof/gc_anomaly_rows.txt:17`, `docs/superpowers/reports/2026-07-26-list-incompleteness-proof/blob_storage_log_three_keys.txt:2`). The proof does **not** identify the store-side mechanism and does not establish the behaviour on AWS S3 (`docs/superpowers/reports/2026-07-26-list-incompleteness-proof/README.md:54`). It therefore supports “the multi-page enumeration can omit durable objects”, not a claim about which individual page or service violated which guarantee.

The commissioner's narrower reading does not make the catalog redundant. `forEachListedKey` stitches a walk from separately fetched pages (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h:369`), and the production backend constructs a fresh iterator with `start_after` for each page (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:1138`). Such a page set need not correspond to the namespace set at any instant. In contrast, `CasRefCatalog::read` obtains and decodes one object version and token (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:19`). Both observations become stale after they are taken, but only the catalog observation defines a cut: creation before the cut is in the body, while creation after it belongs to the already-required temporal argument. That argument protects late ownership through delayed condemnation, rematerialization/ordering, and the delete-site in-degree re-read (`docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:138`). “Both have TOCTOU” is therefore true but not dispositive; staleness after a coherent cut and a scan which may never have represented a cut are different proof obligations.

A bounded re-`LIST` or “retry when an out-of-order key is observed” is not a replacement: two finite scans can agree while both omit an entire namespace, and a whole-namespace omission supplies no observation that triggers the retry. A generation counter can work only if every creator and remover brackets publication, GC accepts only a stable generation, and an abandoned in-progress generation is recoverable. That is a lifecycle catalog with less information but essentially the same `Creating`/fence reconciliation problem. Since the catalog machinery is already landed, replacing it with that protocol before release would add risk without removing the hard part.

## What to keep from what is landed, and what to revert {#what-to-keep-and-what-to-revert}

Keep:

- All Stage A arithmetic-stream, seal, `_ckpt`, durable-hold, and destructive-frontier work. It fixes the observed within-namespace log omission by exact-key arithmetic rather than by trusting listed membership; current intake explicitly uses the listing only for a frozen work bound and computes every interior key (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1849`).

- Task 1b's pure preparation extraction, commit `8d536022eab`. It removes decision logic from the pre-durable half of `CasRefLedger::commitRefChunk` without depending on catalog authority or rebirth.

- Task 2's canonical catalog codec, bounded token-CAS loop, and admission accounting from `87a7fd7c618` and `5150a1cba16`, but amend `CatalogEntry` to remove `incarnation`. The single-object catalog is the valuable part. Its current retry loop already centralizes fresh-read/reapply semantics (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:64`).

- Task 3's `Creating`/`Live`/`Removing` lifecycle states, creator-fence reconciliation, and `_ckpt`-before-`Live` ordering from `5a98ef5c4ae` and `3e1b6358104`. Keep these as catalog-owned birth mechanics, not as responsibilities reimplemented by `CasRefLedger`. The current implementation mints the incarnation at exactly one site (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:218`) and reconciliation changes only the creator fence, not that incarnation (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:245`), confirming that incarnation represents rebirth only, not lease change.

- Task 4-C's catalog-authoritative universe and its fail-closed fixes: one catalog snapshot per GC round, no vacuous damaged-catalog proof, and reuse of that snapshot by destructive consumers. Current code already records why re-reading the catalog during cleanup can apply a plan to a different answer (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h:416`). Keep the fsck correction which puts catalog namespaces into its universe as well.

- Lower-case Task 4c's constant-size `_ckpt` invariant and the later “reject a decreasing `life_epoch`” correction, commits `5c5854ea054`, `f271e42744d`, `59cbe85640d`, and `0d9f08abe79`. Those are stream-integrity improvements independent of rebirth.

- The non-minting read/removal correction `9318862bd5d`. A read or `IF EXISTS` removal must not grow the catalog; `namespaceFilesLifeIfReadable` now expresses absence without a write (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:4125`).

Revert or replace, surgically rather than by blindly reverting mixed commits:

- Remove Task 1/1c's `NamespaceLifeId` identity and incarnation segment from ref and namespace-file keys (`9f9514a90f1`, `e0d333890d6`, `147875b6748`). The type exists solely to make same-name lives disjoint (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasNamespaceLifeId.h:60`). Under the proposed no-reuse rule, `RootNamespace` is the complete identity again. Retain unrelated parser strictness and enumeration exception fixes from those rounds.

- Remove lower-case Task 4b's “rebirth waits for nothing” keying (`827bc0a9189`). Namespace files may return to `roots/<ns>/_files/...`; old files cannot alias a new life because that exact namespace can never have a new life.

- Replace Task 4-C's incarnation-specific implementation: `live_incarnation`, the foreign-incarnation pre-filter, the current-versus-stale incarnation contradiction detector, all `resolveLifeOrSentinel` uses, and the Stage-A sentinel. The catalog-backed namespace **set** remains authoritative. The current filter and its follow-on anomaly machinery occupy `CasGc.cpp:1372-1490`; they are consequences of overlapping lives, not of catalog authority itself.

- Do not mechanically roll format generation 5 back. Task 4-A commit `21617aedda2` contains unrelated fixes and the branch is pre-release/recreate-only. Define one final release grammar at the existing new generation (or bump once more if required), remove the incarnation path component, and regenerate the format goldens once.

- Replace Task 5 rather than executing it. Its present brief has accumulated cursor incarnation, deposited incarnation, warm-cache life invalidation, foreign-life janitoring, same-epoch rebirth, uncataloged-removal discrimination, and multi-removal reservation cases (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1196`, `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1226`, `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1244`). Those are not independent safety necessities; they are the downstream cost of allowing the same `RootNamespace` to become a second life before all evidence of the first has retired.

## The concentration proposal {#the-concentration-proposal}

Make `CasRefCatalog` the only owner of namespace lifecycle decisions, and give GC one small immutable `GcNamespacePlan` built from one catalog snapshot.

`CasRefCatalog::Snapshot` should expose typed operations rather than its raw `entries` vector:

- `lookup(ns) -> Absent | Creating | Live | Removing`;

- `liveAndRemoving() -> vector<RootNamespace>`;

- `ensureLive(ns, creator_fence, admitted_generation)`;

- `beginRemoval(ns, owner_fence)`;

- `isRetired(ns)`, backed by an exact, stable per-namespace retirement marker.

Move all of `CasRefLedger::resolveNamespaceLife` into `CasRefCatalog::ensureLive`; today that roughly hundred-line method separately reads entries, creates, resumes its own creator, reconciles a foreign creator, and maps outcomes (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:920`). Delete `resolveLifeOrSentinel`, `stageATransition`, and `fromCatalogEntry`. Readers call `lookup` and receive absence; writers call `ensureLive`; removers call `beginRemoval`. No caller outside `CasRefCatalog` interprets a raw lifecycle row.

At the GC boundary, `makeGcNamespacePlan(catalog_snapshot, listed_keys, parent_seal)` should return:

- the namespaces to walk;

- retired debris which is cleanup-only;

- catalog inconsistencies which suppress destruction;

- cursor keys to carry or prune.

`Gc::fold`, ref cleanup, namespace cleanup, fsck, and decommission receive that immutable plan or the same immutable catalog snapshot; none performs an independent catalog read. This finishes the direction started by `FoldResult::live_incarnation` without preserving its incarnation-specific representation. It also removes the current repeated resolution in fsck and decommission (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:711`, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp:150`). The present five mechanisms are visibly spread through the writer, GC, orphan sweep, protocol, fsck, and decommission; the backlog's inventory found about 80 references and the current tree still exposes all five concepts (`docs/superpowers/cas/BACKLOG.md:3134`).

The replacement removal sequence is:

1. catalog `Live -> Removing`;
2. the fenced owner appends the terminal record;
3. GC folds the terminal record and performs one bounded cleanup pass;
4. GC writes a stable retirement marker derived only from `RootNamespace`;
5. GC durably publishes a fold seal with that namespace's cursor pruned;
6. GC exact-deletes `_ckpt`;
7. catalog entry deletion is last.

Creation checks the retirement marker and the namespace `_ckpt` by exact key before admission and refuses if either exists. Ordering the retirement marker before `_ckpt` deletion means even catalog loss cannot turn the removal window into a fresh birth. Listed objects found after entry deletion are safe to treat as retired cleanup debris after an exact marker check. A listing omission can leak old objects, but it cannot make them visible again because the namespace is never admitted again. This preserves the fail-close direction without an incarnation, a foreign-life janitor, or a cleanup job capable of targeting a future life.

The write-hotspot evidence is real: 137 of 250 timeout lines named `cas/ref_catalog`, and ordinary stateless tests failed on emitted transient errors (`docs/superpowers/cas/BACKLOG.md:3265`). It is not a reason to discard the coherent cut, especially given the commissioner's stated workload judgment. It is a release gate: rerun the S3 lane after read/removal non-minting and the simplified lifecycle, and do not enable destructive GC if ordinary creation still times out at that rate.

## Invariants to cut {#invariants-to-cut}

- **Cut fresh incarnation per rebirth and incarnation-qualified keys.** It currently buys structural non-aliasing when the exact same `RootNamespace` is removed and recreated while old objects or stale cleanup survive (`CasNamespaceLifeId.h:60`). Without it, exact namespace reuse must fail permanently and clearly. Normal `Atomic` drop-and-create is unaffected because the namespace contains the table UUID and a recreated table gets a fresh UUID (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:1232`, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h:505`). Reusing an old UUID, or a shadow namespace path, breaks: the operator must choose a fresh namespace.

- **Cut “rebirth waits for no physical cleanup” and foreign-incarnation janitoring.** It currently buys immediate same-namespace reuse even if `LIST` hides old `_files` or ref debris (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1018`). Without it, the retirement marker refuses reuse and leftover objects are leak-only. One stable marker per retired namespace is unbounded in historical namespace count, but it replaces the existing permanent cleanup-marker class rather than putting historical rows in the hot catalog; the active catalog remains `O(Creating + Live + Removing)`.

- **Cut incarnation-scoped cursors, deposited-incarnation cleanup, cached-life invalidation, and the current/dead-incarnation contradiction detector.** They currently buy safety when old and new lives overlap. Without overlap, the old cursor is pruned before the entry disappears, cleanup has no possible future target, and no cache can alias a new life. What breaks if the ordering is violated is severe: entry deletion before the cursor-pruning seal would reintroduce a permanent uncataloged cursor and pool-wide suppression, exactly the current Task 5 warning (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1250`). Therefore the ordering remains an executable invariant.

- **Cut admission reservation for repeated removal/rebirth rows.** It currently buys bounded serialization even when one namespace has multiple cleanup rows while no current catalog entry exists (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:1286`). Under no reuse, there is at most one removal lifecycle for a namespace. Reserve for one active catalog row, one cleanup item, and its cursor/hold; any second removal is an exception, not another budget case.

- **Do not cut catalog authority, `Creating` publication exclusion, the `Removing` positive-ownership exclusion, the exact-next frontier probe, durable holds, or the delete-site in-degree re-read.** These prevent data loss independently of rebirth. In particular, a merely temporary “refuse while old objects survive, retry after GC” rule does **not** make incarnation unnecessary: with the observed enumeration defect, `LIST` cannot certify that every old object is gone, and a stale writer can be excluded only by the full fence/terminal argument. Incarnation becomes unnecessary only with permanent exact-`RootNamespace` non-reuse (or with a durable generation token, which is incarnation under another name).

## Cost {#cost}

This is four implementation tasks plus one release gate, not the remaining nineteen tasks of the current plan:

1. state the no-reuse contract, add the exact retirement marker and its failure/recovery tests, and settle the final recreate-only wire grammar;
2. remove `NamespaceLifeId` and incarnation path plumbing while preserving Task 1b and the independent parser/`_ckpt` fixes;
3. concentrate creation/read/removal decisions in `CasRefCatalog`, and build/pass one immutable `GcNamespacePlan`;
4. land the shortened removal sequence, cursor pruning, fsck/decommission use of the shared snapshot, and the destructive-gate tests;
5. run the full CA gates, both object-storage lanes, and a destructive soak.

The work is broad because Tasks 1/1c/4b deliberately made the identity compiler-visible, but it should be net code deletion and removes most of Task 5 plus the incarnation portions of Tasks 5b, 6, 7, and 9. It is worth one focused pre-release week because it trades a rarely used feature for a much smaller proof surface. If the deadline cannot absorb those four code tasks and the gate, the only defensible fallback is to ship with `UniversePolicy` still suppressing destructive GC; hurriedly landing current Task 5 and flipping Task 7b is not an acceptable deadline trade.

## What would change my mind {#what-would-change-my-mind}

I would keep the current incarnation design if a supported, pre-release-critical workflow must reuse the exact same `RootNamespace`—not merely the same SQL table name—and cannot instead mint a fresh table UUID or shadow namespace. That observation would make permanent retirement an unacceptable semantic regression, and incarnation would again buy something necessary.

I would drop the authoritative catalog as universe only after a repeatable, supported-store result and protocol argument establish that a quiescent full paginated `LIST` is complete, and that namespace creation/removal cannot move behind its cursor without a generation bracket which GC validates. The current proof's inability to identify the RustFS mechanism is not that result; at the GC enumeration boundary it observed an omission.

I would replace the single catalog object with a sharded or head-CAS construction if production-rate namespace churn, after non-minting reads and the simplified lifecycle, still produces catalog contention comparable to the 2026-07-31 S3 lane. That would change the catalog representation, not the need for a coherent namespace cut.
