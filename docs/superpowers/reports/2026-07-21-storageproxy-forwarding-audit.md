Verdict: do not land the held diff exactly as written. `checkMutationIsPossible` is correct in `StorageProxy`; `checkTableCanBeRenamed` is correct for `StorageTableProxy` but too broad for generic `StorageProxy`; `checkTableCanBeDetached` is currently unnecessary and its comment is factually wrong. More importantly, the audit finds substantially worse existing holes, including backups of unloaded lazy `MergeTree` tables going through `IStorage::backupData`’s no-op default.

## 1. Held edit

### `checkMutationIsPossible`: correct

The signature exactly matches [`IStorage.h:506`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:506), including `const` and `const Settings &`. Forwarding before `mutate` is required: the inherited implementation unconditionally throws at [`IStorage.cpp:247`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.cpp:247).

Materialization from a `const` check is appropriate here. This is authoritative validation immediately preceding a consequential operation; preserving the nested engine’s policy is more important than preserving laziness. `StorageTableProxy::getNested` is explicitly logically-const lazy initialization protected by its mutex ([`StorageTableProxy.h:37`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageTableProxy.h:37)).

This forward belongs in generic `StorageProxy`: asking whether a mutation is possible already implies use of the underlying storage, including for a table-function proxy.

### `checkTableCanBeRenamed`: semantically required, wrong layer

The signature matches [`IStorage.h:470`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:470), and failing to forward can bypass real restrictions in `StorageReplicatedMergeTree`, `StorageKeeperMap`, and object-storage queues.

However, put it on `StorageTableProxy`, not generic `StorageProxy`. `StorageProxy` also backs `StorageTableFunctionProxy` ([`StorageTableFunction.h:25`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageTableFunction.h:25)); a generic forward makes metadata-only rename of a table-function proxy instantiate and start its underlying storage, potentially contacting an external source. `StorageTableFunctionProxy` deliberately avoids such loading for lifecycle operations ([`StorageTableFunction.h:59`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageTableFunction.h:59)).

For real lazy tables, materialization is desired despite the cost. `DatabaseAtomic` invokes the check while holding its database mutex ([`DatabaseAtomic.cpp:321`](/home/mfilimonov/workspace/ClickHouse/master/src/Databases/DatabaseAtomic.cpp:321), [`DatabaseAtomic.cpp:346`](/home/mfilimonov/workspace/ClickHouse/master/src/Databases/DatabaseAtomic.cpp:346)). That makes hidden construction/startup under the lock undesirable, but silently bypassing the guard is worse. This path deserves a concurrency/deadlock regression test.

### `checkTableCanBeDetached`: do not add currently

The signature matches [`IStorage.h:651`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:651), but the rationale is incorrect:

- `IStorage` says this hook is only for dictionaries.
- The only real override is `StorageDictionary` ([`StorageDictionary.h:68`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageDictionary.h:68)).
- Materialized views do not override it.
- Lazy loading explicitly excludes dictionaries ([`DatabaseOrdinary.cpp:426`](/home/mfilimonov/workspace/ClickHouse/master/src/Databases/DatabaseOrdinary.cpp:426)).
- `InterpreterDropQuery` calls the hook for ordinary tables too ([`InterpreterDropQuery.cpp:280`](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterDropQuery.cpp:280)), so the proposed forward would fully start every unloaded table on `DETACH`, for no current semantic benefit.

If lazy eligibility later includes a storage that overrides this hook, handle it on `StorageTableProxy` or exclude that engine from laziness.

## 2. Complete missing-virtual audit

This inventory is against the held working tree, so the three proposed overrides are not listed. I compared all virtual declarations in `IStorage.h`, including `getAllRegisteredNames`, against the full `StorageProxy` override set at [`StorageProxy.h:19`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageProxy.h:19).

Classification:

- **A** — inherited implementation is currently correct.
- **B** — proxy-visible semantics must delegate to the nested storage.
- **C** — generic forwarding is not clearly right; encode explicit subtype/eligibility policy.

| Missing virtual | Class | Reason |
|---|---:|---|
| [`getInnerStorageIDs`:99](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:99) | C | Compound storages need truth, but current lazy eligibility excludes the only present override; forwarding can defeat lazy drop paths. |
| [`isMergeTree`:101](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:101) | B | Used for routing and optimization; lazy `MergeTree` currently reports false. |
| [`isDataLake`:103](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:103) | B | Engine identity/capability. |
| [`isExternalDatabase`:106](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:106) | B | Engine identity/capability. |
| [`isObjectStorage`:109](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:109) | B | Engine identity/capability. |
| [`isMessageQueue`:112](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:112) | B | Affects ingestion/view behavior. |
| [`isDictionary`:121](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:121) | C | Dictionaries are intentionally excluded from lazy loading; generic forwarding can introduce metadata-time materialization. |
| [`supportsStreaming`:130](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:130) | B | `ReplicatedMergeTree` can return true. |
| [`supportsPartitionBy`:133](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:133) | B | User-visible DDL capability. |
| [`supportsTTL`:136](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:136) | B | Lazy `MergeTree` currently incorrectly reports false. |
| [`getConditionSelectivityEstimator`:141](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:141) | B | Loses engine-specific optimizer support. |
| [`supportedPrewhereColumns`:145](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:145) | B | Nested engines override the unrestricted default. |
| [`canMoveConditionsToPrewhere`:148](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:148) | B | Its default is not equivalent when nested engines explicitly opt out. |
| [`supportsOptimizationToSubcolumns`:172](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:172) | B | Concrete existing bug: proxy inherits `supportsSubcolumns`, while engines such as `IStorageCluster` explicitly opt out. Analyzer consumers are at [`FunctionToSubcolumnsPass.cpp:747`](/home/mfilimonov/workspace/ClickHouse/master/src/Analyzer/Passes/FunctionToSubcolumnsPass.cpp:747). |
| [`supportsTransactions`:177](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:177) | B | Transaction routing capability. |
| [`prefersLargeBlocks`:184](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:184) | B | Execution strategy differs by engine. |
| [`isSystemStorage`:187](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:187) | C | Current lazy tables cannot be system tables; generic forwarding would materialize during attach/accounting. |
| [`areAsynchronousInsertsEnabled`:190](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:190) | B | Insert routing behavior. |
| [`isSharedStorage`:192](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:192) | B | Storage topology/policy. |
| [`tryGetColumnSizes`:200](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:200) | B | The nested optional has three-state semantics; defaulting through `getColumnSizes` loses `nullopt`. |
| [`getSecondaryIndexSizes`:205](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:205) | B | Lazy `MergeTree` incorrectly exposes no index sizes. |
| [`getInMemoryMetadataPtr`:212](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:212) | C | Intentionally proxy-owned cached metadata; forwarding would make routine metadata access materialize tables and conflict with snapshot/conversion behavior. |
| [`getAllRegisteredNames`:225](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:225) | A | Correctly derives names from the proxy’s cached interface metadata ([`IStorage.cpp:337`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.cpp:337)). |
| [`applyMetadataChangesToCreateQueryForBackup`:230](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:230) | B | Backup metadata must preserve engine-specific changes. |
| [`backupData`:233](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:233) | B | **Critical:** inherited default is a no-op ([`IStorage.cpp:405`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.cpp:405)); backup calls it directly ([`BackupEntriesCollector.cpp:879`](/home/mfilimonov/workspace/ClickHouse/master/src/Backups/BackupEntriesCollector.cpp:879)). An unloaded lazy `MergeTree` can therefore contribute no data. |
| [`restoreDataFromBackup`:236](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:236) | B | Must invoke engine restore; default only rejects unexpected files. |
| [`supportsBackupPartition`:239](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:239) | B | Otherwise partition backup/restore is falsely rejected. |
| [`finalizeRestoreFromBackup`:245](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:245) | B | Engine post-restore lifecycle must run. |
| [`supportsLightweightDelete`:248](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:248) | B | Mutation strategy capability. |
| [`supportsLightweightUpdate`:251](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:251) | B | Otherwise the proxy reports the inherited rejection. |
| [`hasProjection`:254](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:254) | B | Query planning capability. |
| [`supportsDelete`:258](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:258) | B | User-visible mutation capability. |
| [`supportsSparseSerialization`:261](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:261) | B | Storage/serialization planning capability. |
| [`supportsTrivialCountOptimization`:266](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:266) | B | Otherwise valid count optimizations are disabled. |
| [`getSerializationHints`:272](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:272) | B | Loses engine-collected serialization statistics. |
| [`tryGetSerializationHints`:275](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:275) | B | Must preserve the nested optional/failure semantics. |
| [`addInferredEngineArgsToCreateQuery`:279](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:279) | B | Required for `File`, `URL`, and object-storage engines to persist inferred/resolved arguments. |
| [`needRewriteQueryWithFinal`:366](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:366) | C | Current override belongs to `StorageMaterializedPostgreSQL`, which is not eligible here; revisit if eligibility broadens. |
| Private [`Pipe read`:389](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:389) | A | The public query-plan overload is overridden and delegates directly; the inherited public implementation that calls this private hook is bypassed. |
| [`parallelizeOutputAfterReading`:406](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:406) | B | Public advisory behavior can differ independently from `isSystemStorage`. |
| [`distributedWrite`:445](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:445) | B | Default silently disables an engine execution path. |
| [`dropInnerTableIfAny`:455](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:455) | C | Blind forwarding would materialize every lazy table during `DatabaseAtomic::dropTable`; current inner-table engines are deliberately eagerly loaded. |
| [`updateExternalDynamicMetadataIfExists`:496](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:496) | B | Data-lake/object-storage metadata refresh must reach the engine. Proxy metadata synchronization also needs explicit handling. |
| [`updateLightweight`:537](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:537) | B | Inherited implementation throws. |
| [`executeCommand`:542](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:542) | B | Inherited implementation throws; object-storage engines implement it. |
| [`waitForMutation`:547](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:547) | B | Inherited implementation throws despite nested mutation support. |
| [`setMutationCSN`:549](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:549) | B | Mutation state must reach the nested engine. |
| [`killPartMoveToShard`:552](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:552) | B | Inherited implementation throws for replicated tables. |
| [`onActionLockRemove`:600](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:600) | B | Directly explains the `START` half of the action-lock incident: callers invoke it on the proxy at [`InterpreterSystemQuery.cpp:309`](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterSystemQuery.cpp:309). |
| [`tryGetDataPaths`:661](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:661) | B | Must preserve nested `nullopt`, notably for aliases. |
| [`tryGetStoragePolicy`:667](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:667) | B | Same optional-semantic problem. |
| [`isStaticStorage`:671](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:671) | A | Current implementation derives entirely from forwarded `getStoragePolicy` ([`IStorage.cpp:378`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.cpp:378)); no engine currently overrides it. |
| [`totalRowsByPartitionPredicate`:682](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:682) | B | Loses exact partition-aware count support. |
| [`totalBytesUncompressed`:706](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:706) | B | System-table accounting becomes empty. |
| [`tryLifetimeRows`:714](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:714) | B | Must preserve nested optional semantics. |
| [`tryLifetimeBytes`:722](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:722) | B | Must preserve nested optional semantics. |
| [`getStorageSnapshotWithoutData`:731](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:731) | B | `MergeTreeData` supplies engine-specific snapshot data even in this mode ([`MergeTreeData.h:668`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.h:668)); the proxy default loses it. |
| [`initializeDiskOnConfigChange`:737](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:737) | B | Already-materialized nested storage otherwise misses disk reconfiguration. |

The nested `DataValidationTasksBase` virtuals at lines 615–616 are not `IStorage` methods and are excluded. The implicit virtual destructor inherited through `IHints` needs no proxy override.

The three most urgent additions beyond the held mutation fix are `backupData`/restore support, `supportsOptimizationToSubcolumns`, and `onActionLockRemove`. The backup hole is serious enough that I would not consider the feature production-safe without testing it.

## 3. Design alternatives

### a. Keep the proxy and add a guard

There is no useful pure-C++ compile-time mechanism that automatically says “a new base virtual was not overridden”:

- Inherited virtuals satisfy the type system.
- `final` prevents further overriding; it does not require an override to exist.
- `-Woverloaded-virtual` detects hiding, not omission.
- A pointer-to-member/static-assert test still requires manually naming every method.
- Vtable inspection is ABI-, compiler-, thunk-, and optimization-dependent and cannot reliably distinguish intentional inheritance from an omission.

A reasonable test-time guard is a small Clang AST-based CI check that:

1. Enumerates every virtual `CXXMethodDecl` on `IStorage`.
2. Enumerates exact overridden declarations on `StorageProxy`.
3. Fails for any new difference not present in a checked-in allowlist.
4. Requires each allowlisted A/C method to carry a rationale.

Complement that with a fake nested storage contract test exercising all B methods. The AST check catches interface growth; the runtime test catches forwarding the wrong arguments or calling the wrong overload. Neither catches direct `dynamic_cast` consumers, so an explicit ban/check for casts on catalog-returned `StoragePtr` would also help.

### b. Swap the database map entry on materialization

As a correctness solution, this does not work well.

The database owns a `map<String, StoragePtr>` ([`DatabasesCommon.h:65`](/home/mfilimonov/workspace/ClickHouse/master/src/Databases/DatabasesCommon.h:65)), but pointers escape through:

- table-iterator snapshots copied under the database mutex ([`DatabasesCommon.cpp:434`](/home/mfilimonov/workspace/ClickHouse/master/src/Databases/DatabasesCommon.cpp:434));
- query-context storage caching ([`DatabaseCatalog.cpp:1178`](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/DatabaseCatalog.cpp:1178));
- the UUID map, which directly stores the `StoragePtr` ([`DatabaseCatalog.cpp:365`](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/DatabaseCatalog.cpp:365), [`DatabaseCatalog.cpp:921`](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/DatabaseCatalog.cpp:921));
- action-lock bookkeeping keyed by `table.get()` ([`ActionLocksManager.cpp:39`](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/ActionLocksManager.cpp:39)).

Every pre-swap holder retains the proxy, so forwarding remains mandatory. Worse, `IStorage`’s share/alter/exclusive locks are nonvirtual object-local state ([`IStorage.h:294`](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/IStorage.h:294)); old callers would lock the proxy while new callers lock the nested object. The same split applies to `is_dropped`, `is_detached`, and restart state. Swapping therefore risks two synchronization domains for one table.

Making the swap compare-and-replace safely also interacts poorly with `DatabaseAtomic` calling virtual checks while holding its database mutex. At best, swapping reduces how often the bug appears; it does not remove the bug class.

### c. Better long-term boundary

The clean design is lazy materialization at the database/catalog entry boundary, before any `StoragePtr` escapes:

```text
catalog table entry: parsed metadata + once/future + optional real StoragePtr
                                      |
getTable / UUID lookup ---------------+--> materialize --> return real StoragePtr
system metadata enumeration ----------+--> use parsed metadata without StoragePtr
```

That requires changing `DatabaseWithOwnTablesBase::tables` and the UUID mapping to refer to a table entry rather than directly caching a proxy `StoragePtr`. It is a real refactor, but it restores exact dynamic type, one pointer identity, and one lock domain.

A less invasive architectural alternative is engine-owned lazy startup: construct the real storage type at database startup and let expensive engines lazily load parts/background state internally. That preserves RTTI and the complete interface but may recover less startup time, depending on where construction cost actually lies.

## 4. Recommendation

The current `lazy_load_tables` proxy is net-negative for upstream-quality generic storage code: it is an opt-in startup optimization implemented by weakening type identity and duplicating storage state across an interface with roughly sixty unforwarded virtuals, and it already appears capable of silently omitting backup data. The cheapest robust path is to keep the setting disabled and not promote it upstream; land only the mutation fix now, place the rename check specifically on `StorageTableProxy`, omit the detach hook, then either quarantine the feature behind a complete forwarding audit plus Clang-AST guard or remove it until catalog-entry laziness is available. Given the incident history, I would choose disable/quarantine rather than continue fixing failures one virtual at a time. No files were modified.
