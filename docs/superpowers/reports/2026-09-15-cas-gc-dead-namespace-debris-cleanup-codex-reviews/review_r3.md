## Summary

The overall direction is sound: do S1, then bounded S2 at the existing phase position, retain the global janitor, and consider A only after resolving its recovery-consumer race. Rev.3 correctly fixes the five rev.2 MAJOR findings, but introduces one new factual contradiction in the interpretation of the 37 no-read rows.

## MAJOR findings

### 1. The 37 no-read rows do distinguish valid-triple empty plans

> “A valid triple can also produce an empty plan …; no row in this sample distinguishes that from the 37”

This is contradicted by the call path. Once a checkpoint has `checkpoint_snapshot_id`, `cleanupRefObjects` calls `readCheckpointSnapshotBase` before `planRefCleanup` ([CasGc.cpp:3644](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:3644)). That validation necessarily reads at least the same-ID `_log` and `_snap` ([CasRefProtocol.cpp:970](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:970), [CasRefProtocol.cpp:1028](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:1028)).

All 37 rows have zero `CASRootGet`. Therefore none reached successful—or failed—triple validation. Of those rows, 36 have nonzero `namespaces_planned`; for every planned namespace, execution stopped at the absent-checkpoint or absent-`checkpoint_snapshot_id` checks. The remaining row planned zero namespaces.

This does not identify short-lived tables, but it does exclude “valid triple followed by empty plan” as an explanation for those 37 rows.

Fix: replace the quoted sentence with: “The 37 rows prove that no planned namespace reached triple validation; they do not distinguish absent `_ckpt` from a checkpoint without `checkpoint_snapshot_id`, and they do not establish which workload cohort those namespaces belong to.”

## MINOR findings

- **The 40 rows localize the stop to catalog revalidation, not specifically the ETag comparison.**

  > “`authorityHolds` returned false at the catalog comparison”

  One `CASOtherGet` and no `CASGCGet` proves execution entered `authorityHolds` but stopped before the `gc/state` read. It cannot distinguish ETag/row comparison failure from an absent, undecodable, or ambiguous catalog, because the entire catalog read and validation block is caught together ([CasGc.cpp:3586](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:3586)). Say “stopped during catalog revalidation before `gc/state`”; describe ETag churn as the likely hypothesis requiring a reason counter.

- **The zero-delete full-page range is slightly wrong.**

  > “0.208-0.835 s”

  In the fixed 1,843-line prefix, the maximum is 0.844691 s at [cas_gc_log.tsv:1647](/home/mfilimonov/workspace/ClickHouse/lane-g/tmp/investigation/t4/msan_local/run8/samples/cas_gc_log.tsv:1647). The correct range is 0.208–0.845 s. The overall full-page range of 0.208–8.849 s is correct.

- **A failed S1 chunk is not necessarily revisited by the next page.**

  > “leaves the remainder for a later page”

  Under the current cursor rule, caught per-key deletion failures do not make the page undecided; the cursor still advances to `page.next_cursor` ([CasNamespaceJanitor.cpp:129](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.cpp:129), [CasNamespaceJanitor.cpp:148](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.cpp:148)). If S1 preserves that behavior while continuing mutable-key handling, failed remainder is retried only after cursor wrap. Say “a later traversal,” and test the wrap.

- **S2 needs an explicit result for its progress/end condition.**

  `NamespaceJanitorResult` currently exposes neither `end_of_pass` nor whether cursor publication committed ([CasNamespaceJanitor.h:10](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h:10)). A caller otherwise cannot distinguish end-of-pass, publication conflict, or retained cursor and may process the same page repeatedly. Add explicit `cursor_committed` and `end_of_pass` results and stop the per-round loop on either failed progress or end-of-pass. The proposed “time budget” also needs definition; only the two page-count settings are currently specified.

- **The snapshot threshold wording is off by one.**

  > “the snapshot trigger is 256 transactions or 1 MiB”

  The code uses strict `>` comparisons ([CasRefLedger.cpp:4164](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:4164)). Say “exceeds thresholds whose defaults are 256 transactions or 1 MiB.”

## Verified critical contracts

- `CatalogLifeIndex` indexes every lifecycle state. `resolve` returns the unique logical life, returns absent only when the physical ID is missing, and throws on duplicate ownership ([CasRefProtocol.cpp:23](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:23)).
- `retired_lives` contains lives observed absent or replaced, not proof that this actor erased them. It can be present in an overall `FencedOut` result ([CatalogLifecycleReconciler.cpp:99](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CatalogLifecycleReconciler.cpp:99)).
- The regular drain runs before DEFER, global ref enumeration, and fold ([CasGc.cpp:469](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:469)); force rebuild also invokes it before its hot enumeration ([CasGc.cpp:4212](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:4212)).
- The drain already performs irreversible conditional catalog replacement/row erasure ([CasRefCatalog.cpp:514](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:514)), but no physical namespace cleanup.
- `CasRequests::admit` merely captures the fence generation and callback. `gate` samples that callback before requests/reissues; it neither refreshes GC authority nor cancels an in-flight request ([CasRequests.cpp:304](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp:304), [CasRequests.cpp:413](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp:413)).
- `deletePrefixWholesale` counts visited keys, including benign `Gone`, `Mismatch`, and absent-HEAD cases; `out_fully_drained` means enumeration ended within budget, not that a re-LIST would be empty ([CasGc.cpp:3733](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:3733)).
- The rev.3 recovery correction is accurate: missing `_ckpt`, committed logs, or unchanged sampled bases can raise non-transient `CORRUPTED_DATA`. A remains correctly blocked on defining and testing caller behavior.

## Answers

The recommended order is sound with its stated qualification: S1, then S2 at the current post-fold phase position, measure throughput, and only then A after resolving the recovery-exception/interleaving contract. Keep the global page permanently.

The “`planRefCleanup` returns early without a checkpoint ⇒ short-lived tables leak every log” finding is only conditionally correct. The helper does return an empty plan without a checkpoint, but production normally stops even earlier on a missing snapshot ID or failed triple validation. Thus a life without a validated recovery triple retains every log through covered-ref cleanup. “Short-lived tables” are not proven to be that population, and the janitor means this is retention by that cleanup path, not permanent leakage.

VERDICT: MAJOR: 1
