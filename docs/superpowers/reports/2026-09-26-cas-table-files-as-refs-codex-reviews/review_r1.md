# Codex review round 1 of the design study rev.1 (gpt-5.6-sol, high) {#codex-review-round-1}

Reviewed commit: 2f0eccc7975 (rev.1: file contents as refs, dedup claims). Verdict: REVISE; the cache alternative judged better; rev.2 pivoted to directory entries in the ref log. Findings verbatim:

## Alternatives

- [MAJOR] **§7 Q1 — Prefer the reviewed striped cache for the immediate problem.** It touches only `RefTableRuntime`, lookup/admission paths, and cache accounting. It requires no format migration, no MergeTree change, and no new GC/TLA+ state. It does not achieve literal “one storage model,” but that goal does not justify the migration risk here. The competing design has already converged through six review rounds; this one has several unresolved correctness issues.

- [MAJOR] **§7 Q1 — If one model is mandatory, use typed refs, not name inference.** Add `RefKind { Directory, File }` to committed ref state, log records, and snapshots; reserve table-level subtrees in `PartPathParser`; use a constant internal manifest path such as `data` for file refs. This touches the CAS format, parser, codecs, fsck, and TLA+ model, but classification remains resident and exact. A kind bit inside the manifest is worse because every `existsFile`/`existsDirectory` probe may need a manifest fetch. Current committed rows contain no kind: `CasRefSnapshotFormat.h:37-45`.

- [MINOR] **§7 Q1 — A singleton table-state ref is cleaner conceptually but worse operationally.** One manifest could hold all table files. It removes name ambiguity, but every mutation or dedup update rewrites the same hot manifest and produces more garbage. It also serializes unrelated metadata changes.

- [MAJOR] **§7 Q2 — There is no existing MergeTree deduplication extension point.** The concrete type is included, returned, and owned by `StorageMergeTree`: `src/Storages/MergeTree/StorageMergeTree.h:17,129,162`; construction is at `StorageMergeTree.cpp:1508-1521`. The proposed extraction therefore touches the header and ownership/accessor types, not merely one implementation-choice line.

- [MAJOR] **§7 Q2 — Zero upstream touch is possible: keep `MergeTreeDeduplicationLog` and represent its files as file refs.** The existing `createFile`/append/move/remove operations can be satisfied by the CAS disk. This gives one storage model without an interface. It retains a manifest repoint and immutable log object per write, so it is less efficient, but it is the smallest and most rebase-portable option.

- [MINOR] **§7 Q2 — Branching inside the concrete `MergeTreeDeduplicationLog` would reduce call-site changes but is not cleaner.** It leaves `MergeTreeSink` and `StorageMergeTree` unchanged, but couples MergeTree directly to CAS pool internals and spreads CAS conditions through upstream code. A small interface/factory is preferable if claims are retained.

- [MAJOR] **§7 Q3 — The cleanest claim design is claim-plus-part publication in one ref transaction.** Pass block IDs through the part commit context; the serialized ledger builder checks collisions, records claims, and publishes the part atomically. It prevents phantom claims after failed inserts and can genuinely share one durable transaction. It requires larger `MergeTreeSink`/part-publication changes, so it conflicts with the minimal-upstream objective.

- [MAJOR] **§7 Q3 — The smaller acceptable shape is a typed, durable claim journal in ref state.** Use explicit operations such as `ClaimBlockIds`, `ReleaseBlockIds`, and `SetClaimWindow`, with duplicate checking inside the serialized builder and durable FIFO eviction per block ID. This keeps `MergeTreeSink` source-compatible through an interface, but cannot share the inserting part’s PUT.

- [MAJOR] **§7 Q3 — Claims attached only to their part ref are not sufficient.** Merges remove source refs while their deduplication claims must survive. Transferring every source claim to every merge output would expand MergeTree touch substantially. Separate marker objects or a separate bounded journal introduce another storage/GC model and are worse.

- [CRITICAL] **§7 Q4 — One format bump works only as a pool-wide maintenance migration.** Fence all generation-1 mounts, migrate and validate every live server root, raise the pool floor, then admit generation-2 writers. “First node finishes and raises the floor” is unsafe. Splitting the change across generations 2 and 3 merely creates two migrations.

- [MAJOR] **§7 Q5 — Preserve current claim-before-publish ordering only if minimal upstream touch wins.** Then the design must accept a separate durable claim transaction. If elegance and request reduction win, move the duplicate check into atomic claim-plus-part commit. The present design claims the benefits of both, but its call ordering supplies neither.

## Risk

- [CRITICAL] **§4.1 No downgrade — real; mitigation missing.** The floor is checked during pool decode, not continuously by already-mounted nodes: `CasPoolMetaFormat.cpp:155-158`. An old node mounted before the floor change can remain active. Generation-2 objects are stamped as soon as a generation-2 build writes them: `CasFormat.h:13-21,61-67`. A global mount fence and lease-renewal gate are required.

- [CRITICAL] **§4.2 Migration interruption — real; stated idempotence is false.** Existing `publishEntries` rejects a different manifest already bound to the destination unless repointing is enabled: `PartFolderAccess.cpp:467-474`, `CasPartWriteTxn.cpp:954-973`. A failure after publication but before deleting `_files` wedges retry unless migration explicitly resolves and content-compares the existing destination.

- [CRITICAL] **§4.3 Deduplication history — real; mitigation violates the hard constraint.** Skipping a corrupt segment with a warning is a fallback and may permanently discard claims when the source is later deleted. The migration must fail closed and preserve the source. `kMaxImportedClaims` can also silently shorten a configured window.

- [CRITICAL] **§4.4 Claim growth — real; runtime trimming is insufficient.** Window size and FIFO advancement must be durable. Otherwise snapshot/replay resurrects claims removed by `ALTER`, including after a zero-size window. The bound must count block IDs, not claim operations.

- [CRITICAL] **§4.5 Name-based split — real and already disproved.** It misclassifies `partition_exports` and `tmp_restore_<part>-<random>`; see §3.1.

- [MAJOR] **§4.6 Upstream drift — real; scope understated.** The interface changes `StorageMergeTree.h`, construction, result types, build dependencies, and probably the CAS access/factory boundary. A CAS implementation under `Disks` depending directly on `MergeTreePartInfo` is a layer inversion.

- [MAJOR] **§4.7 Append conflicts — low risk for current mutation callers, but the claimed contract is false.** Mutation CSN writes are serialized by `currently_processing_in_background_mutex`: `StorageMergeTree.cpp:1123-1132`. Generic `repointRef`, however, has no expected-old-manifest condition, so concurrent appenders are last-writer-wins: `CasPartWriteTxn.cpp:954-973`.

- [CRITICAL] **Missing risk — multi-node pools and dead-node namespaces.** A floor is pool-wide while migration is described per server root. The first finisher can exclude old builds before sibling roots are migrated. Later generation-2 nodes also skip migration if it is triggered only by `floor == 1`. Dead but cataloged roots need explicit migration or exact-life decommission; absent roots may be left to the janitor.

- [MAJOR] **Missing risk — GC, janitor, fsck, and TLA+.** New operation kinds require changes to `RefTableState::applyOp`, scope validation, state-growing admission, namespace removal, codecs, snapshots, and models: `CasRefProtocol.cpp:367-425,720-776`; `CasRefLedger.cpp:3138-3153,3181-3194,3363-3367`. Fsck must distinguish legitimate claims whose original part ref no longer exists.

- [MAJOR] **Missing risk — snapshot growth.** Claims enlarge every full ref snapshot; snapshots are republished by count/byte thresholds and are capped at 64 MiB: `CasPool.h:277-291`. The proposal has no sizing or admission analysis for large windows.

- [MAJOR] **Missing risk — backup, restore, and FREEZE.** Logical backup synthesizes part/mutation entries rather than copying raw table metadata: `StorageMergeTree.cpp:3471-3510`. Restore creates `tmp_restore_<part>-XXXXXXXX`: `MergeTreeData.cpp:7785-7794,7887-7908`, which the proposed naming rule misclassifies. Pool-level backup must preserve the floor and every root’s migration state atomically.

- [MAJOR] **Missing risk — hot-key lane.** `appendRefOps` waits for durability before returning: `CasRefLedger.cpp:2040-2179`. `MergeTreeSink` invokes `addPart` before part publication: `MergeTreeSink.cpp:381-407`. A claim and its own part cannot normally share a queued PUT.

- [MAJOR] **Missing risk — irreversible automatic activation.** A generation-2 binary appears to migrate on first mount without an operator gate, preflight, dry run, or maintenance-mode requirement.

## §3.1 File-ref mapping

- [CRITICAL] **Name classification is incomplete.** Table-level paths include:

  - `format_version.txt`: `MergeTreeData.cpp:508-559`.
  - `tmp_mutation_<n>.txt` and `mutation_<n>.txt`: `MergeTreeMutationEntry.cpp:22-25,51-64,89-95,111-116`.
  - `deduplication_logs/deduplication_log_<n>.txt`: `MergeTreeDeduplicationLog.cpp:69-72`; selected at `StorageMergeTree.cpp:1514`.
  - `partition_exports/<hash>.json.tmp` and `<hash>.json`: `MergeTreePartitionExportScheduler.cpp:55-63,706-749`.
  - Live part refs and `tmp_insert_`, `tmp_merge_`, `tmp_mut_`, `tmp_clone_`, `tmp_empty_`, `tmp-fetch_`, `tmp_replace_from_`, `tmp_move_from_`, `delete_tmp_`, `tmp_restore_...`, `detached/...`, `moving/...`, and shadow/FREEZE refs.

  `PartPathParser` currently treats `partition_exports` as a part ref because only `deduplication_logs` is reserved: `PartPathParser.cpp:188-197,257-273`. The proposed rule instead calls it a file, breaking directory probes and iteration. `tmp_restore_<part>-<random>` does not satisfy `looksLikePartDir`: `PartPathParser.cpp:136-168`. Every `detached/*` and `moving/*` entry should be directory-typed regardless of whether its tail is valid part grammar.

- [MAJOR] **`existsFile` / `existsDirectory`: conditionally viable.** O(1) resident lookup requires a committed `RefKind`; name inference is not correct. Cold recovery may still be required, so “always memory-only” is too strong.

- [MINOR] **`readFile` / `getFileSize`: viable.** Resolving the ref and reading the cached inline/blob manifest follows existing behavior: `ContentAddressedMetadataStorage.cpp:1741-1767,2035-2066`.

- [MAJOR] **Directory probes/listing: viable only with preserved prefix routing.** `partition_exports` proves nested table-level directories remain. `DirShape::TableSubdir` cannot simply disappear; it may be renamed, but equivalent routing is still required.

- [MAJOR] **Rewrite mapping is wrong.** Default `publishEntries` refuses rebinding an existing ref to different content: `CasPartWriteTxn.cpp:954-971`. Rewrite needs atomic create-or-repoint semantics.

- [MAJOR] **Append mapping overstates conflict detection.** `repointRef` force-resolves and then overwrites whichever binding exists inside the serialized closure; it does not condition on the binding that was read: `PartFolderAccess.cpp:508-544`, `CasPartWriteTxn.cpp:954-973`.

- [MAJOR] **Unlink mapping loses API semantics.** `removeFile` must fail if absent while `removeFileIfExists` must not: `IDiskTransaction.h:93-97`. Mapping both to `dropRefIfPresent` is incorrect.

- [CRITICAL] **Move/replace breaks the manifest entry path.** `republishRef` copies entries unchanged: `PartFolderAccess.cpp:477-505`. Moving `tmp_mutation_7.txt` to `mutation_1.txt` leaves the old entry path, so destination lookup by last component fails. Use a constant internal name such as `data`.

- [MAJOR] **Replace is not implemented by `republishRef`.** It rejects an existing destination with different content: `PartFolderAccess.cpp:488-499`. Missing source returns `false`; callers must translate that into `FILE_DOESNT_EXIST`.

- [MINOR] **Recursive remove/table rename/drop can use prefix operations**, but absent/error semantics and partial retry must be specified. Namespace removal must also clear or validate claims; current removal only considers ownership state.

## §3.2 Claims

- [CRITICAL] **Duplicate checking has a TOCTOU race.** “Check under `state_mutex`, then append” allows two writers to pass the check. Holding the mutex while calling `appendRefOps` risks self-deadlock. The duplicate test and claim insertion must occur inside the serialized ledger builder, which operates on the working state: `CasRefLedger.cpp:3043-3045`.

- [CRITICAL] **Window semantics are not preserved durably.** Upstream maintains a FIFO bounded by individual block IDs; `setMaxSize` immediately evicts entries: `MergeTreeDeduplicationLog.h:27-117`. A runtime-only trim is lost after restart. `SetClaimWindow` and resulting evictions must be committed to the log/snapshot.

- [MAJOR] **Release semantics are underspecified.** Upstream `dropPart` removes every entry whose stored `part_info` is contained by `drop_part_info`, not merely entries equal to one ref name: `MergeTreeDeduplicationLog.cpp:308-359`. `ReleaseClaims(ref_name)` must define whether it preserves this range behavior.

- [MINOR] **Merge behavior should preserve claims.** Current merge paths do not call `dropPart`, which is correct. DROP/DETACH callers do: `StorageMergeTree.cpp:2529-2536,2655-2657,2921-2924`.

- [CRITICAL] **`ALTER MODIFY SETTING` and restart recovery are incomplete.** The setter is invoked at `StorageMergeTree.cpp:833-841`; implementation is `MergeTreeDeduplicationLog.cpp:364-383`. Shrinks, zero-window transitions, and later increases must not resurrect evicted claims.

- [CRITICAL] **The request-count claim is false.** Because `addPart` durably waits before part publication begins, its claim cannot “usually share one PUT” with that part. Expect at least one immutable claim log object per unbatched insert, plus snapshots. Preserve the required HEAD-before-PUT protocol and measure the real count.

- [MAJOR] **Large `ClaimBlockIds` operations may exceed the existing per-operation size limit.** Current log operations are limited to 4096 encoded bytes: `CasRefLogFormat.h:101-112`. Splitting must preserve all-or-nothing duplicate detection.

- [MAJOR] **The interface extraction can leave `MergeTreeSink.cpp` source unchanged only if the interface owns `AddPartResult` and the accessor returns the interface.** `StorageMergeTree.h` and construction still change. The design must also identify the factory, CAS pool/lifetime access, and dependency direction.

## §3.3 Migration

- [CRITICAL] **The floor protocol is unsafe.** The first node must not raise the pool floor. Generation-2 objects must not be written until all generation-1 mounts are fenced and all live roots are migrated or explicitly decommissioned.

- [CRITICAL] **Per-root migration cannot be keyed solely by the pool floor.** After one root raises floor 2, another generation-2 node will observe floor 2 and, under the stated trigger, skip its remaining `_files`. This contradicts test 14. Persist per-root migration completion independently.

- [CRITICAL] **An old build mounted on another root remains dangerous during migration.** It may continue writing generation-1 metadata and can run pool-wide GC/fsck against generation-2 objects. Refusal at a future mount does not protect already-mounted processes.

- [MAJOR] **Read-only generation-2 mounts are undefined.** On a floor-1 pool they cannot perform migration, while no compatibility reader is allowed. They must fail with a specific migration-required error.

- [CRITICAL] **File publication is not idempotent as described.** Retry must resolve the destination ref and compare canonical file content: equal is success, different is corruption. “Byte-equal publish is already a no-op” is not true of the cited primitive.

- [MAJOR] **Source deletion needs an exact-token condition.** Capture the source object identity during read and delete only that version after publication. Otherwise retry can delete a replacement.

- [CRITICAL] **Corrupt deduplication segments must abort migration.** Upstream may warn and continue during ordinary load: `MergeTreeDeduplicationLog.cpp:129-148`; migration cannot copy that fallback and then delete the only source.

- [CRITICAL] **`kMaxImportedClaims` is not a valid correctness bound.** It may be below the configured window, and the CAS pool does not know the table setting at mount. Either import all claims and durably trim when the actual setting is installed, or migrate after table settings are available.

- [MAJOR] **“No `_files` under any life” conflicts with migrating only cataloged live lives.** Define whether dead/absent debris blocks completion. The sensible rule is: migrate every live life; delete absent-life debris through the janitor; require explicit handling for cataloged but decommissioned lives.

- [MAJOR] **Startup ordering is missing.** Foreground access, GC, fsck repair, and the janitor must remain closed until migration and validation complete.

## Tests

- [CRITICAL] **Wrong order:** establish and test the multi-node upgrade fence before enabling any generation-2 writer or migration. Current tests begin from per-node migration assumptions that are already unsafe.

- [CRITICAL] **Test 14 contradicts the algorithm.** A sibling cannot migrate “independently” after another node raises the only migration trigger from floor 1 to floor 2.

- [MAJOR] **Request-profile test 11 asserts an impossible claim-plus-part shared PUT.** Replace it with measured protocol assertions for the actual call ordering, including required HEAD-before-PUT steps.

- [MAJOR] **Missing path cases:** `partition_exports`, `tmp_restore_*`, invalid detached names, `_tryN` detached suffixes, `moving`, shadow/FREEZE, projections, and table rename with file refs.

- [MAJOR] **Missing operation-contract cases:** rewrite an existing file; replace an existing destination; move from absent source; strict versus if-present unlink; nested directory probes; append lost-update detection.

- [CRITICAL] **Missing claim cases:** simultaneous identical inserts, multiple block IDs in one insert, operation-size splitting, window shrink/grow/zero across restart, crash before/after snapshot, DROP PARTITION range release, DETACH, and merges preserving old claims.

- [CRITICAL] **Missing migration fault injection:** every boundary between read, claim replay, ref publication, exact source deletion, per-root marker, and floor change; retry after destination publication; corrupt input retention; two roots; dead/absent roots; already-mounted old node.

- [MAJOR] **Missing subsystem coverage:** snapshot-size limits, resident weight, GC, janitor, fsck, TLA+ transitions, long-running claim churn, read-only mounts, decommission, logical BACKUP/RESTORE, pool snapshot restore, and FREEZE/UNFREEZE.

## Ambiguities and contradictions

- [MAJOR] **§3.1:** “manifest path equals the file’s last component” contradicts move and replace.
- [MAJOR] **§3.1:** “nothing nested remains” contradicts `partition_exports`.
- [MAJOR] **§3.2:** claim journal bounds do not say whether the unit is an operation, part, or block ID.
- [MAJOR] **§3.2:** “no object/no garbage per insert” contradicts synchronous durable `appendRefOps`.
- [MAJOR] **§3.2:** `ReleaseClaims(ref_name)` does not define containment/range semantics.
- [MAJOR] **§3.3:** “first node raises the floor” contradicts independent sibling migration and safe rolling upgrade.
- [MAJOR] **§3.3:** “skip corrupt segment” contradicts the no-fallback constraint.
- [MINOR] **§3.3:** `kMaxImportedClaims` is an unspecified placeholder, not a protocol rule.
- [MINOR] **Scope:** the 400–600 line estimate omits exhaustive protocol switches, snapshot/admission accounting, fsck, TLA+, `StorageMergeTree.h`, migration coordination, and tests.

## Verdict

The cache alternative is the better path now: it addresses the observed directory-probe cost without a format migration or MergeTree dependency. This design is unlikely to converge in two revisions unless claims are postponed and the scope is reduced to exact typed file refs under a coordinated maintenance migration.

**REVISE**
