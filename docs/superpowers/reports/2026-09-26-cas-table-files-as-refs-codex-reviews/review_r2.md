# Codex review round 2 of the design study rev.2 (gpt-5.6-sol, high) {#codex-review-round-2}

Reviewed commit: d4f9ea346ec (rev.2: directory entries in the ref log). Verdict: REVISE; the striped runtime cache judged the better path; convergence would need two more revisions. Findings verbatim:

## Alternatives

- [MAJOR] §7 Q1 — A mount-populated, write-through runtime table already meets every explicit §2 request bound: one LIST per live table at mount and no later LIST while the runtime remains resident. It avoids a format bump, pool floor, snapshot changes, GC exposure, and durable reconciliation. It loses survival across runtime eviction, offline inspection, and persistence between mounts; pinning it for the mount lifetime addresses the first loss.

- [MAJOR] §7 Q1 — The reviewed striped cache remains the smaller path. Its stripe is also needed here to serialize LIST/PUT/DELETE with table updates; the durable design does not eliminate that concurrency requirement. Durability provides little request benefit because §3.3 unconditionally repeats the LIST at every mount.

- [MINOR] §7 Q1 — A reserved `_files` ref avoids codec changes but is worse: every membership change rewrites/repoints one hot manifest, introduces conflict handling and garbage, and exposes `_files` as an ordinary ref to generation-1 readers.

- [CRITICAL] §7 Q5 — Explicit empty directories and LIST-derived reconciliation are incompatible. `createDirectory` can durably add an empty directory, but the next resync removes it because no `_files/<dir>/...` object supports it. Choose one:
  - implied directories, with no persistent empty-directory promise;
  - marker objects for explicit directories;
  - or distinct `ExplicitDirectory`/`ImpliedDirectory` states and asymmetric reconciliation.

- [MAJOR] §7 Q5 — Pure implied directories require adding the first component on the first nested PUT and removing it when the last child disappears. The latter requires a LIST, child counts, or explicit recursive-removal semantics. It is simpler if empty directories are deliberately unsupported.

- [MAJOR] §7 Q5 — Recording every nested name would give exact directory semantics and remove nested LISTs, but the deduplication log would append a ref-log operation per insert, recreating rev.1’s rejected hot-path cost.

- [MAJOR] §7 Q2 — “One LIST” means one full paginated prefix enumeration, not one backend request. On large deduplication histories its work is proportional to every nested object, not the number of top-level entries.

- [MAJOR] §7 Q3 — A safe floor can ignore deleted/decommissioned mount slots, but must block on every nonterminal generation-1 slot and every unreadable slot. The current subtree contains `/owner` and `/epoch` objects too; existing enumeration deliberately selects only `/mount`: `Pool/CasServerRoot.cpp:1035-1048`.

- [MAJOR] §7 Q4 — Generation-1 readers fail closed per transaction/namespace, but the GC round does not globally abort and “apply nothing.” Details are under Generations.

## Round-1 status

### Still or partially applicable

- [CRITICAL] Round 1 §7 Q4 — STILL, narrowed: a format bump remains pool-wide and needs exact active/dead/decommissioned-root rules; rev.2’s all-mount rule fixes the “first node raises it” defect for visible live slots.

- [CRITICAL] Round 1 no-downgrade finding — STILL: generation-2 writes exclude generation-1 readers; already-mounted generation-1 nodes are safe only while their renewable generation-1 mount slot reliably blocks the floor.

- [CRITICAL] Round 1 name classification — STILL: `partition_exports` remains misclassified, and non-Atomic `tmp_restore_*‑XXXXXXXX` still fails the part grammar.

- [CRITICAL] Round 1 multi-node/dead-root finding — STILL: deleted mount slots, terminal slots, corrupt slots, and decommissioned roots are not specified.

- [MAJOR] Round 1 GC/fsck/TLA+ surface — STILL: the affected surface is larger than §3.4/§6 state.

- [MAJOR] Round 1 snapshot growth — STILL with directory rows replacing claims; admission and the 64 MiB snapshot cap need analysis.

- [MAJOR] Round 1 backup/restore/FREEZE coverage — STILL: rev.2 lists none of their exact path classifications.

- [MAJOR] Round 1 irreversible activation — STILL: a writable generation-2 mount automatically emits irreversible state without a gate or preflight.

- [MAJOR] Round 1 directory routing — STILL: `TableSubdir` routing remains necessary, and `partition_exports` proves the current parser is insufficient.

- [MINOR] Round 1 recursive-remove/rename retry contracts — STILL: partial failures and absent-object handling remain underspecified.

- [MAJOR] Round 1 read-only mount finding — STILL: “does not consult the table” does not define generic read-only `IDisk` directory APIs.

- [MAJOR] Round 1 startup ordering — PARTLY ADDRESSED by “before serving,” but mount admission, ref recovery, resync batching, floor update, and tool/GC startup order remain unstated.

- [CRITICAL] Round 1 test-order finding — STILL: generation safety and crash semantics must precede enabling writers.

- [MAJOR] Round 1 missing path, operation-contract, and subsystem tests — STILL, except claim-specific cases.

- [MINOR] Round 1 size-estimate finding — STILL: serialization, mount orchestration, parser repair, fsck audit, batching, and floor machinery are omitted.

### Moot

- [MAJOR] Round 1 file-per-ref/manifest-repoint alternatives — MOOT; rev.2 keeps file contents in `_files`.

- [MAJOR] Round 1 `MergeTreeDeduplicationLog` abstraction and upstream-touch findings — MOOT.

- [CRITICAL] Round 1 claim atomicity, FIFO, release, growth, operation-size, and request-count findings — MOOT.

- [CRITICAL] Round 1 destructive content migration, retry, content comparison, exact source deletion, corrupt-segment import, and `kMaxImportedClaims` findings — MOOT.

- [MAJOR] Round 1 file-ref rewrite, append, unlink, move-entry-path, and replace semantics — MOOT as stated; rev.2 has new physical-object/directory-op ordering races instead.

- [MAJOR] Round 1 hot claim lane and claim-plus-part publication findings — MOOT.

## §3.2 operation mapping

- [MINOR] `listDirectory(<table>)` — Viable as ref names plus directory entries, but it must retain the authoritative empty-result check at `ContentAddressedMetadataStorage.cpp:1842-1861` and define collisions between refs, reserved containers, and directory names.

- [MAJOR] `existsFile(<table>/<name>)` — A resident lookup can replace GET, but only after same-name object/table mutations are serialized. Current code performs the exact object GET at `ContentAddressedMetadataStorage.cpp:1493-1505`; under the proposed crash/resync rules the table can become stale.

- [MAJOR] `existsDirectory(<table>/<name>)` — The lookup is viable, but routing must consult known directory entries before arbitrary post-UUID names are classified as parts. The current `TableSubdir` LIST is at `ContentAddressedMetadataStorage.cpp:1702-1712`.

- [MINOR] Nested list/exists and all nested exact reads — Correctly remain LIST/GET operations once routing is fixed.

- [MAJOR] Top-level create — PUT followed by add is the correct creation direction; the current finalize callback only performs PUT at `ContentAddressedTransaction.cpp:848-852`. A crash after PUT is recoverable by resync.

- [CRITICAL] Existing top-level rewrite — “No op: the entry exists” is unsafe without same-name serialization. A rewrite can decide no add while a concurrent unlink appends removal and deletes; the later PUT then leaves an unrecorded object indefinitely.

- [MAJOR] Nested write — “No op” works only if the directory entry already exists. A direct nested PUT into a new first component is invisible until remount. Implied-directory semantics require adding that first component after PUT.

- [CRITICAL] Directory creation — The real production callers use `createDirectories`, which reaches `createDirectoryRecursive`, currently a separate no-op at `ContentAddressedTransaction.cpp:1015-1018`. The mapping changes only `createDirectory` at `:1007-1013`. `MergeTree` uses recursive creation for `detached`, `deduplication_logs`, and `partition_exports`.

- [MAJOR] `removeDirectory` — Current non-part behavior is simply no-op: `ContentAddressedTransaction.cpp:1020-1059`. Removing the table entry without proving emptiness hides surviving children until resync; local-disk empty-directory semantics require an emptiness check.

- [CRITICAL] Top-level unlink ordering — `DirEntryRemoved` before DELETE satisfies “no entry names a missing object” during the call but is not crash-safe with §3.3. A failure after the op but before DELETE leaves an object which the next resync re-adds, resurrecting the deleted file. Test 5’s claim that resync ignores it is false. Use DELETE-before-remove, or add a durable tombstone protocol. Current object removal is `ContentAddressedTransaction.cpp:1648-1665`.

- [MAJOR] Strict versus if-present unlink — The existing HEAD/GET branch distinguishes the contracts and must remain; the directory table must not replace that check when strict absence semantics matter.

- [CRITICAL] `moveFile`/`replaceFile` — Current order is GET, destination PUT, source DELETE at `ContentAddressedTransaction.cpp:1472-1485`; `replaceFile` delegates at `:1565-1580`. Correct interleaving requires destination PUT→destination add and source DELETE→source remove. “Same, plus ops” cannot append both after the helper. The source-absent/destination-present retry branch at `:1476-1481` must also reconcile both entries.

- [MAJOR] `removeRecursive(<table>/<dir>)` — Current behavior LISTs and deletes children at `ContentAddressedTransaction.cpp:1153-1167`. Delete children before removing the entry; failure before the final op leaves a stale empty directory, which exposes the explicit-directory/resync contradiction.

- [MAJOR] Table rename — Current code republishes refs, LISTs files, GET/PUTs each file, then drops the source namespace: `ContentAddressedTransaction.cpp:1265-1304`. Destination entries must be added after each successful PUT; source entries disappear atomically with `RemoveNamespace`. The `if (auto bytes)` skip at `:1300-1301` is a fallback and must instead propagate missing-source failure.

- [MAJOR] Drop — `RemoveNamespace` must clear the directory table atomically. Current `applyOp` requires only empty committed/precommit owner sets and does not know directory entries: `Pool/CasRefProtocol.cpp:387-403`.

- [CRITICAL] Ordering as a whole — No two-step object/ref-log protocol can simultaneously guarantee “entry never names a missing object” and crash-safe absence without a tombstone. The spec must select and model the tolerated transient state.

- [CRITICAL] Concurrency — Ref-lane ordering alone is insufficient because PUT/DELETE occur outside the lane. Operations on the same top-level name, plus the mount LIST, need a stripe held across object I/O and ref-state mutation. Different-name commutativity does not solve same-name races.

## Produced table paths

- [MINOR] `MergeTree` refs — Live parts and `tmp_insert_*`, `tmp_merge_*`, `tmp_mut_*`, `tmp_clone_*`, `tmp_empty_*`, `tmp-fetch_*`, `tmp_replace_from_*`, `tmp_move_from_*`, and `delete_tmp_*` are refs. Files and projections inside them are manifest entries, not directory-table entries.

- [MAJOR] Restore — Atomic `tmp_restore_<part>-XXXXXXXX` is a temporary part ref, later republished as the final part; its contents are manifest entries: `MergeTreeData.cpp:7785,7887-7949`. Non-Atomic parsing does not recognize the random suffix as part grammar: `Parts/PartPathParser.cpp:136-168`.

- [MINOR] Containers — Top-level `detached/` and `moving/` are reserved containers; `detached/<part>` and `moving/<part>` are refs, not directory-table entries.

- [MINOR] `MergeTree` table files — `format_version.txt`, `tmp_mutation_<n>.txt`, and `mutation_<n>.txt` are directory-table `File` entries: `MergeTreeData.cpp:508-559`; `MergeTreeMutationEntry.cpp:25,57`.

- [MINOR] Deduplication — `deduplication_logs` is a directory-table `Directory`; `deduplication_logs/deduplication_log_<n>.txt` are nested `_files` objects, not table entries.

- [MAJOR] Partition exports — `partition_exports` is intended to be a directory-table `Directory`; `<hash>.json.tmp` and `<hash>.json` are nested objects. It is currently parsed as a part because only `deduplication_logs` is exempted after an Atomic UUID: `Parts/PartPathParser.cpp:188-197,257-273,322-328`. Rev.2 does not work for this path without CAS-318-like routing changes.

- [MINOR] `Log` — Every serialization stream `<stream>.bin`, `__marks.mrk`, and `sizes.json` is a directory-table `File`: `StorageLog.cpp:53-54,729,793-800`.

- [MINOR] `TinyLog` — Every serialization stream `<stream>.bin` and `sizes.json` is a directory-table `File`; there is no `__marks.mrk`.

- [MINOR] `StripeLog` — `data.bin`, `index.mrk`, and `sizes.json` are directory-table `File` entries: `StorageStripeLog.cpp:313-315`.

- [MINOR] Logical backup — Atomic `BACKUP` reads part storage and creates backup-archive entries; it creates no live-table directory entry. Non-Atomic temporary-hardlink backup on CAS is unsupported. Backup archive paths are neither refs nor live directory entries.

- [MINOR] FREEZE — `shadow/<backup>/.../<part>` is a part ref in a shadow namespace; intermediate `shadow` paths are containers, and files/projections remain manifest entries: `MergeTreeData.cpp:10337-10420`.

## §3.3 resync

- [CRITICAL] It is not sound as written: it resurrects objects left by remove-before-delete failures and deletes legitimate explicit empty-directory entries.

- [MAJOR] Resync is a reasonable one-time migration and drift detector only after the authority rule is fixed. With LIST as authority, object deletion must precede entry removal; with log state as authority, reconciliation must retain tombstones and cannot blindly add every listed object.

- [MAJOR] “Single ref transaction” fails above 5,000 changed entries: `Formats/CasRefLogFormat.h:101-112`. Reconciliation needs bounded batches, idempotent restart, and a mount barrier held until every batch commits.

- [MAJOR] Resync must run after ref recovery and mount fencing, before any disk/API exposure, and under the same namespace-file stripe as all later PUT/DELETE operations. Otherwise LIST and a concurrent writer can commit contradictory results.

- [MAJOR] Large-pool cost is understated. It is one full `_files` enumeration per live table, including every nested dedup segment, with pagination and memory proportional to returned names. Startup latency and backend throttling need measured bounds.

- [MAJOR] Read-only mounts need one explicit policy: retain current LIST behavior, build a non-durable runtime table from one LIST, or reject directory APIs/mounts requiring the table. “Tools use objects, not directories” does not define read-only `IDisk` semantics.

## §3.4 generations

- [MINOR] Decoder atomicity — An unknown op is rejected while decoding the full transaction at `Formats/CasRefLogFormat.cpp:367-406`; no operation from that transaction reaches `applyOp`.

- [MAJOR] GC behavior — The fold catches decode/extraction failure, installs a per-namespace hold, and stops that namespace at `Gc/CasGc.cpp:2625-2644`. It may already have folded earlier transactions and continues other namespaces. “The round aborts and applies nothing” is false.

- [MAJOR] Orphan sweep behavior — `activeManifestKeys` decodes before deriving protection at `Gc/CasOrphanManifestSweep.cpp:368-395`; callers catch failure and skip all deletions for that namespace at `:585-613` and `:823-876`. It continues other namespaces. This is fail-closed per namespace, not a whole-sweep abort.

- [MAJOR] Expected error — Generation-1 normally fails `openObject` on the generation-2 object header with `UNKNOWN_FORMAT_VERSION`, before `refOpKindFromWireWord`; every generation-2 object is stamped with compatibility generation 2 by `Formats/CasFormat.cpp:60-77`. `CORRUPTED_DATA` applies only to directly decoding an unwrapped unknown op body.

- [MAJOR] Format registration — Bumping `G_BUILD` is insufficient. Breaking change points for `RefLog`, `RefSnapshot`, `MountLease`, and every newly written format must be appended in `Formats/CasFormat.cpp:24-55`.

- [MAJOR] Floor membership — The rule must say: nonterminal generation-1 slots block; unreadable/unknown mount objects block; terminal, fenced, or explicitly decommissioned slots do not; deleted slots do not. Existing enumeration ignores `/owner` and `/epoch` and treats removal of `/mount` as removal from membership: `Pool/CasServerRoot.cpp:1026-1048,1064-1077`.

- [MAJOR] Completion marker — A generation-2 mount object proves reader generation, not successful directory resync. A failure after claiming the mount but before completing resync leaves a generation-2 marker. The floor must be documented as excluding old readers only, or a separate resync-complete marker is required.

- [MAJOR] Rolling upgrade — Until the floor reaches 2, generation-1 GC/fsck/inspect can encounter generation-2 objects and fail closed per namespace. After the floor reaches 2, generation-1 mounts fail at pool decode: `Formats/CasPoolMetaFormat.cpp:155-158`.

- [MAJOR] Downgrade contradiction — §3.3 says a generation-1 build may write and later be repaired; §3.4 says generation 1 cannot mount a root after generation-2 state exists. Both cannot describe the supported rollout.

## Format surface

- [MAJOR] Ref-log codec — Extend the enum/DTO and every wire switch in `Formats/CasRefLogFormat.h:33-69` and `Formats/CasRefLogFormat.cpp:36-95,141-210,395-418`, including removal-class classification.

- [MAJOR] Snapshot codec — “A section” is not the current wire shape. Snapshots are one sequence of kind-discriminated rows plus one trailer count: `Formats/CasRefSnapshotFormat.cpp:118-136,140-278`. Define the row grammar, `entry_kind`, ordering, duplicate rules, trailer count, and exact row-size helper.

- [MAJOR] State protocol — Extend state storage, `applyOp`, `stateFromSnapshot`, `snapshotOf`, debug counter checks, and exact budget functions: `Pool/CasRefProtocol.cpp:367-468,610-631,698-735`; `Pool/CasRefProtocol.h:192-274`.

- [MAJOR] Namespace removal — Either `RemoveNamespace` clears directory entries implicitly or its removal-class transaction must name them. The latter would enlarge drop transactions; the former is simpler but must be included in snapshot accounting and invariants.

- [MAJOR] Scope validation — A directory entry needs a dedicated scope or an explicitly namespaced reuse of `MutationScope::Ref`. Current scope validation recognizes only owner transitions and `SetPublishedAt`: `Pool/CasRefLedger.cpp:2730-2750,3133-3153`.

- [MAJOR] Admission — `DirEntryAdded` is state-growing and must participate in preview, positive-append admission, snapshot byte caps, and cache weight: `Pool/CasRefLedger.cpp:3181-3194,3363-3367`.

- [MAJOR] Tooling — `Tools/CasInspect.cpp` must render both op kinds and snapshot rows. `Tools/CasFsck.cpp` needs a physical `_files` versus directory-table comparison, not just printing.

- [MAJOR] Fsck claim — Current fsck classifies `_files` keys as life-owned at `Tools/CasFsck.cpp:529-555`, protects them while the life is cataloged at `:557-591`, and walks only manifest reachability at `:601-705`. It cannot currently report either object-without-entry or entry-without-object.

- [MAJOR] Models — At minimum extend `CaRefTableSnapshotLogCore.tla` and `CaRefDeltaIntakeCore.tla`; also model `RemoveNamespace` clearing and unknown-generation holds where applicable. “TLA+ op set” is not a complete change list.

- [MAJOR] Size estimate — 600–700 production lines is not credible once same-name serialization, mount orchestration, reconciliation batching, parser repair, physical fsck audit, read-only behavior, floor updating, snapshot accounting, and tests are included. The proposed one-and-a-half to two weeks is not supported.

## Tests

- [CRITICAL] Wrong order — First settle and test deletion crash semantics, empty-directory authority, same-name serialization, and multi-node floor behavior. Codec happy paths must not precede unresolved protocol invariants.

- [CRITICAL] Test 5 is wrong: after remove-before-delete failure, the stated resync re-adds the object; it does not ignore it.

- [MAJOR] Test 9 cannot derive an explicit empty directory from LIST. An empty table can be derived as having no entries, but an empty `deduplication_logs` directory cannot.

- [MAJOR] Test 11 expects the wrong integration error and wrong GC scope: framed generation-2 objects yield `UNKNOWN_FORMAT_VERSION`; GC holds one namespace rather than aborting the entire round.

- [MAJOR] Add fault injection after every PUT/add/DELETE/remove boundary for create, unlink, move, replace, recursive removal, and table rename, followed by remount.

- [MAJOR] Add same-name rewrite-versus-unlink/move/replace races and LIST-versus-writer races; also prove different-name batching remains safe.

- [MAJOR] Add `createDirectoryRecursive`, empty-directory restart, direct nested write, last-child removal, nonempty `removeDirectory`, and allocation failure during table update.

- [MAJOR] Add paginated LIST, more than 5,000 differences, failure between reconciliation batches, mount retry, and maximum snapshot-size cases.

- [MAJOR] Add strict/if-present unlink, source-absent/destination-present move retry, replace-existing, same-path move, and partially completed table rename.

- [MAJOR] Add `partition_exports` tmp/final rename and restart, Atomic and non-Atomic `tmp_restore_*`, `detached`, `moving`, projections, logical backup/restore, FREEZE/UNFREEZE, and broken-restore-to-detached.

- [MAJOR] Add `TinyLog`; test `Log`, `TinyLog`, and `StripeLog` create, append, truncate/recreate, rename, restart, and drop.

- [MAJOR] Add fsck drift tests in both directions and ensure no repair path guesses an answer.

- [MAJOR] Add floor cases for live generation-1, expired but nonterminal, fenced, terminal, deleted/decommissioned, corrupt/unknown mount, owner/epoch without mount, and failure after generation-2 mount claim but before resync.

- [MAJOR] Add mixed-op transactions with a known op before an unknown op, proving decoder transaction atomicity and per-namespace GC/sweep retention.

## Ambiguities and contradictions

- [CRITICAL] §3.2 versus §3.3: remove-before-delete plus LIST-authoritative resync resurrects deleted files.

- [CRITICAL] §3.2 versus §3.3: explicit empty directories are removed at the next mount.

- [MAJOR] §3.1: “no coordination beyond the ref lane” ignores object I/O outside that lane.

- [MAJOR] §3.3: “single transaction” ignores `ref_txn_max_ops == 5000`.

- [MAJOR] §3.3/§3.4: downgrade repair is promised even though downgrade mounting is declared unsupported.

- [MAJOR] §3.4: “every server root” is undefined for absent, dead, fenced, terminal, and decommissioned `/mount` slots.

- [MAJOR] §3.4: mount generation is treated as resync completion without specifying their ordering or a completion marker.

- [MAJOR] Snapshot “section” has no wire grammar, row kind, count rule, or backward-read definition. Generation 2 necessarily must read generation-1 snapshots that lack it, despite “no compatibility read path.”

- [MAJOR] Canonical validation for entry names, separators, reserved names, part-looking names, ref collisions, and `File`↔`Directory` replacement is unspecified.

- [MAJOR] Parser/routing changes required for `partition_exports`, non-Atomic `tmp_restore_*`, and future table subdirectories are outside the stated implementation surface.

- [MAJOR] Read-only mount behavior is a placeholder.

- [MINOR] “One LIST” conflates one logical enumeration with one backend request.

- [MINOR] §3.5 says top-level `TableSubdir` branches can be removed, but equivalent directory-aware routing remains mandatory.

## Verdict

Rev.2 is materially smaller than rev.1, but the striped runtime cache remains the better path. This design is unlikely to converge in one revision; it could converge in two only by adopting serialization, fixing deletion authority, dropping or marking explicit empty directories, and fully specifying the floor.

REVISE
