No new core data-loss counterexample survives the churn, incarnation, or slot-occupy analysis. The remaining issues are specification, implementation-closure, and test-plan fixes.

### Findings

1. **Major — the “pool-wide-held baseline” alternative is not representable or clearable as specified.**

   **Claim attacked:** missing/undecodable prior seal produces a pool-wide-held baseline or refusal ([spec §5](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:128)).

   **Scenario/code:** the available shape is per-namespace `ShardCoverage`; there is no pool-level destructive hold in either [`CasFoldSeal`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:91) or [`GcState`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h:30). With an unreadable prior seal, REBUILD knows neither which namespace was held nor its offending position. Synthesizing ordinary classification-4 entries invents positions: an absent invented position can never be “folded through,” while clearing on its absence violates the stated hold-clear rule. No operator-clear authority is defined.

   **Smallest fix:** choose the already-safe branch: missing or undecodable prior seal **must refuse REBUILD**, naming pool recreation as the recovery path. If fold-without-delete progress is required, separately define a top-level, non-auto-clearable pool hold and an authoritative operator repair transition.

2. **Major — the temporal lemma states a false universal writer-rematerialization premise.**

   **Claim attacked:** an already-delete-pending blob is safe because `Condemned` meta forces the writer to rematerialize from source ([spec §5](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:121)).

   **Scenario/code:** source-backed/tokened adoption does check meta and rematerialize ([`observeAndAdmit`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:349)), but relink uses tokenless [`adoptEvidence`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:781), performing no `HEAD`, meta read, or copy. Its safety instead follows from the receiver’s durable precommit preceding source release ([`prepareAdoptFromManifest`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:2151)) and, inductively, the existing source ownership edge. I found no data-loss interleaving once that ordering is included, but the normative proof currently omits an actual writer path.

   **Smallest fix:** split the lemma: source-backed/tokened writes rematerialize; tokenless relinks inherit a committed source edge and durably establish the target edge before source removal. Add a delete-pending, post-frontier tokenless-relink go-red control.

3. **Moderate — the ref-layer read contract is normative but not closed at the API boundary.**

   **Claim attacked:** every ref reader holds `(namespace, incarnation)` and therefore returns stale-or-`NotFound`, never new-life data ([spec read contract](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:81)).

   **Scenario/code:** every current key constructor accepts only `RootNamespace` ([`refsNamespacePrefix`, `refLogKey`, `refSnapshotKey`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h:110)). Namespace-only composition exists in recovery ([`CasRefLedger`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:470)), fold ([`CasGc`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1471)), fsck ([`CasFsck`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:216)), and sweep ([`CasOrphanManifestSweep`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp:225)). Leaving any such overload permits an implementation to reacquire the current incarnation from a stale namespace and alias rebirth.

   **Smallest fix:** introduce one typed `RefNamespaceId{namespace, incarnation}`, require it for every ref prefix/key/parser/cache operation, and delete namespace-only ref helpers. Recovery, fold, fsck, and sweep derive it from the catalog; live reads derive it from their handle; wedge resolution retains the already-qualified key.

4. **Moderate — round 8’s hold grammar/capacity disposition is only partially folded.**

   **Claim attacked:** strict classification-4 grammar plus additive worst-case reservation makes every seal encodable ([spec INV-3 and §5](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:64)).

   **Scenario/code:** v9 says `{reason, offending position, retry/backoff}` but gives no wire keys, reason enum/string bound, numeric maxima, duplicate-field rule, or exact escaping/framing reservation. Current coverage has no such fields ([`ShardCoverage`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:32)), and its decoder is explicitly field-strict ([`decodeFoldSeal`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.cpp:179)). The write-side size check prevents an unreadable seal, but an underestimated reservation can make all subsequent fold-seal writes refuse after catalog admission, wedging GC/removal progress.

   **Smallest fix:** define exact fields and bounded values—prefer a reason enum—and specify the byte-exact worst-case reservation including escaped namespace text, framing, newline, and trailer growth. Test the exact accepted boundary and boundary-plus-one.

5. **Moderate — the stopping criterion misses several load-bearing sabotages.**

   **Claim attacked:** every property has a `_sab_*` configuration and the named controls cover rounds 5–8 ([spec §9](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:176)).

   **Scenario/code:** the listed tests do not explicitly force:

   - missing/undecodable prior seal to reject rather than publish a deletion-capable baseline;
   - a held higher witness to disappear while the gap remains;
   - one ref key builder to drop the incarnation;
   - an old-generation wedge result to return after the successor has adopted/sealed the slot.

   The last ordering matters because current wedge installation after the exact `GET` has no post-I/O generation check ([`CasRefLedger`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1432)); current recovery similarly installs after its unlocked seal I/O ([`installRecoveryResult`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:661)).

   **Smallest fix:** enumerate these controls. Require a generation recheck under the install lock immediately before every recovery or wedge state install/unwedge. Assert one durable application and that the superseded runtime cannot publish `_ckpt` or snapshot state afterward.

6. **Minor — §8 undercounts requests and the catalog asymptotic claim omits `Creating`.**

   **Claim attacked:** the catalog is `O(live + in-flight-removing)` and §8 is an honest cost ledger ([spec INV-3 and §8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:55)).

   **Scenario/code:** `Creating` entries necessarily exist before `_ckpt`, including stalled creators. An occupied `slot-occupy` cannot obtain existing bytes/token from `putIfAbsent`: conflicts return neither ([`PutResult`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h:86)), so each adopted straggler also needs an exact `GET`. Removal costs at least `Live→Removing` CAS, terminal append, exact `_ckpt` deletion, and catalog removal CAS, not one catalog CAS.

   **Smallest fix:** state `O(Live + Creating + Removing)` and add the occupied-slot `GET` and full removal sequence to §8.

### Closed core attacks

- **U1/U2:** catalog-snapshot churn is safe: old and reborn namespaces have different incarnation prefixes, newly admitted ownership falls under the temporal lemma, and cleanup revalidates before deletion. The janitor cannot eat a newborn `_ckpt`: `Creating` is catalog-visible before `_ckpt` is written; a listed `_ckpt` therefore has a visible catalog incarnation.
- **U3:** ordinary carried holds permit folding while suppressing deletion and clear through adopted progress. Only the missing-authority “pool-wide baseline” branch is undefined.
- **U4:** no proposed path needs namespace-only composition, but finding 3 must make that mechanically enforceable.
- **U5:** no durable double-apply exists: old PUT wins → successor adopts it; successor seal wins → old PUT is rejected; generation changes before send → old send is refused. Finding 5 pins the required post-I/O install ordering.
- **U7:** no core/register contradiction found. R2/R3 and R5 are explicitly named same-rollout dependencies; R1 remains correctly limited to the unqualified file layer.

### Round-8 dispositions

1. **REBUILD hold carry:** Confirmed for a readable prior seal; refuted as complete for the unrepresentable missing/undecodable “pool-wide-held” branch.
2. **Hold-clear definition:** Confirmed—only folding through the offending position and adopting it in `gc/state` clears.
3. **Disappeared-witness rule:** Confirmed textually; its dedicated go-red control is still missing.
4. **Temporal lemma:** Partially confirmed; present, but it omits tokenless `adoptEvidence`.
5. **Hold grammar + seal-size check:** Partially confirmed; conditional grammar and write-side size check are present, exact fields/maxima/reservation are not.
6. **Read contract:** Confirmed as stale-or-`NotFound`, never alias, for the ref layer; implementation must close namespace-only APIs.
7. **Wedge wording:** Confirmed—retry is caller/remount-driven, not autonomous.
8. **Migration notes:** Confirmed—writer generation, backward floor, fail-closed startup, and quiesced recreation are explicit.
9. **Decommission branches:** Confirmed registered in R5, including `_ckpt`-present/absent handling and final catalog check.
10. **Nomination retry wording:** Confirmed registered in R3 as “safe to retry when rediscovered,” without a false guaranteed-retry claim.

**VERDICT: APPROVE-WITH-FIXES**
