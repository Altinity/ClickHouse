Both overrules fail under source-level counterexamples. O1 lacks a real completion-before-recovery ordering, and O2’s settlement premise is false in the current build lifecycle.

1. **Blocker — O1’s in-flight chronology is not a protocol guarantee.**

   **Claim attacked.** §2 says every ordinary same-node `PUT` settles before recovery lists the namespace.

   **Proof.** The favorable part is real: CAS selects a single-attempt S3 client, and the SDK retry strategy returns false, so no hidden retry layer resends after the controller stops running ([S3ObjectStorage.cpp:892](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:892), [Client.cpp:196](/home/mfilimonov/workspace/ClickHouse/master/src/IO/S3/Client.cpp:196)). But the controller’s five-second `attempt_timeout_ms` is explicitly only a scheduling check, not a socket deadline ([CasRequestControl.h:136](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h:136)); once admitted, it synchronously enters `backend->putIfAbsent` and cannot interrupt it when the fence later closes ([CasRequestControl.cpp:315](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:315)). The actual S3 send/receive timeout is independently configurable ([diskSettings.cpp:166](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/ObjectStorages/S3/diskSettings.cpp:166)), while self-remount waits only `attempt_timeout + margin` for lanes and then an independently configurable grace ([CasRefLedger.cpp:843](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:843), [CasPool.cpp:1018](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1018)).

   A request admitted just before fence expiry can therefore remain blocked through claim and grace, recovery can miss it, and the same already-started request can complete later. No connection-pool retry is needed. Process death likewise provides no source-backed bound on when a request already accepted by the server becomes visible.

   **Smallest fix.** Reinstate a point-readable per-namespace recovery authority. A timing-only alternative would require an enforced end-to-end request deadline plus a documented maximum server materialization delay and `grace` exceeding both; the present S3 contract supplies neither.

2. **Blocker — O1’s §5b detector is neither eventual nor preventive.**

   **Claim attacked.** A ghost folded under rule 1 will later be detected at an anchor crossing.

   **Proof.** Let the pre-recovery cursor be `A`, ghost log `V` have `prev=A`, and recovery seal `S` declare `(A,S]` void. If a hot fold lists `V` but no post-recovery successor, rule 1 folds `V`. Nothing references `S`, so no anchor crossing ever occurs if the namespace remains quiet. The cursor can authorize destructive work indefinitely without §5b running.

   If both `V` and successor `Y(prev=S)` appear in one sorted round, `V` is processed first. Rule 1 folds it; when `Y` later reveals `S`, §5b examines the round’s `parent_cursor=A`, not the now-resolved `V`, so its strict-inside test does not fire. Rule 3 also specifies only `resolved==sealed_from` and `resolved<sealed_from`; `A<resolved=V<S` is undefined. This contradicts the promised test that a ghost with `prev==cursor` is certified void ([spec:301](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:301)).

   **Smallest fix.** Discover the latest recovery authority before rule 1. Independently, make `sealed_from < resolved_through < seal_id` an immediate breach that clamps to the pre-interval cursor and keeps destructive suppression asserted. The latter does not solve the no-successor case by itself.

3. **Blocker — O2’s “grants settle before the floor passes” premise is false.**

   **Claim attacked.** Every owner grant below `min_active` has settled before the paired `R*`.

   **Proof.** `precommitAdd` marks its state `Uncertain` before entering `appendRefOps` ([CasPartWriteTxn.cpp:965](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:965)). If that append returns unresolved, the transaction can unwind while the grant may have landed. Its destructor nevertheless retires the build unconditionally ([CasPartWriteTxn.cpp:119](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:119)); `retireBuildSeq` immediately erases it from the active set ([CasMountRuntime.cpp:181](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp:181)). The next heartbeat can advance `min_active`.

   If “high-water” means settled/applied IDs, that unresolved grant may later become durable above `R*`. If it means highest allocated/attempted ID, the inequality can survive, but v3 never says that and its settlement proof remains wrong.

   **Smallest fix.** Define `R*` as the inclusive highest allocated/attempted ref ID, read `min_active` first and then the allocator high-water, and require grant ID allocation to happen-before build retirement. Alternatively, keep unresolved builds active until their owner duty is definitively settled.

4. **Blocker — O2 has no safe cross-epoch initialization.**

   **Claim attacked.** Recording `R*` at floor transitions covers every build already below the new mount’s floor.

   **Proof.** A fresh lease currently starts with `min_active=0` ([CasServerRoot.cpp:267](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:267)), while the sweep immediately considers every prior-epoch build eligible ([CasOrphanManifestSweep.cpp:263](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp:263)). The old lease and any old `R*` are overwritten. v3 does not specify what the new lease records before its first current-epoch build finishes.

   `{0,0}` as a present bound is unsafe because every cut satisfies it; “missing” is safe but disables reclamation. It is also not a valid real `RefTxnId` under the current ID contract ([CasTypes.h:265](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h:265)).

   **Smallest fix.** On epoch `E` claim, atomically publish either an explicitly absent bound that always retains, or `{E−1, UINT64_MAX}` covering every dead epoch. Define the first-epoch case and monotonic replacement rules explicitly.

5. **Major — A member-wide `R*` gives safety by retaining, but often never permits deletion.**

   **Claim attacked.** One pool-wide ref high-water is the identifier-space link the namespace sweep needs.

   **Proof.** Ref IDs are type-comparable, but namespace chains contain arbitrary global-counter gaps. If namespace `N` last appended ID 5, another namespace advances the member allocator to 1000 before the relevant build floor transition, then `R*=1000`. A quiet `N` remains chain-verified only through 5 forever, so every orphan manifest in `N` is retained until `N` writes again or a later remount seal dominates 1000. Recovery timing differences across namespaces produce the same stall.

   **Smallest fix.** Record a per-namespace grant high-water or a per-manifest grant certificate. Otherwise document that the proposed bound effectively disables sweep for quiet namespaces—the behavior O2 intended to overrule.

6. **Major — N7’s REBUILD quiescence proof is circular with today’s objects.**

   **Claim attacked.** REBUILD can prove every member lease expired or fenced before trusting its listings.

   **Proof.** Existing lease discovery itself uses `LIST(gc/server-roots/)` ([CasServerRoot.cpp:554](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:554)). Under v3’s hot-LIST trust boundary, an omitted live member makes “all members fenced” false. There is no fixed membership registry or pool-wide read-only fence. Meanwhile REBUILD discovers namespaces through another LIST ([CasGc.cpp:2393](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2393)) and condemns every physically listed blob absent from that reconstructed universe ([CasGc.cpp:2739](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2739)).

   **Smallest fix.** Require a durable pool-wide maintenance lease/fence checked by mount, renew, and mutation paths. Until it exists, specify that REBUILD always refuses—not that it may infer quiescence from lease enumeration.

7. **Major — N3’s sticky floor is stored in non-sticky state.**

   **Claim attacked.** The recovery requirement persists in the runtime across touches.

   **Proof.** Ref-table cache pressure erases entire runtimes ([CasRefLedger.cpp:829](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:829)), and self-remount marks every runtime superseded and clears the map ([CasRefLedger.cpp:929](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:929)). The next touch constructs a fresh runtime with no remembered floor. There is also a race during the unlocked recovery-seal `PUT`: remount can detach the runtime, after which recovery reacquires its mutex and installs without a final supersession check ([CasRefLedger.cpp:629](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:629), [CasRefLedger.cpp:665](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:665)).

   **Smallest fix.** Keep required floors in ledger-level state outside the evictable runtime, transfer them across remount, and reject recovery installation if `superseded_by_remount` became true.

8. **Major — N6’s admission cap has no linearizable admission authority and does not bound tombstones.**

   **Claim attacked.** A new `remove_namespace` is refused when the GC `Pending` set is at its cap.

   **Proof.** Writer admission currently recovers the namespace and immediately appends removal ([CasRefLedger.cpp:2890](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2890)). The `Pending` item does not exist until GC later folds that log ([CasGc.cpp:1689](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1689)). Multiple writers can therefore all observe a below-cap seal and append before any reservation appears. Moreover, limiting `Pending` does not limit permanent lineage tombstones, while the fold-seal format has a hard 256 MiB cap ([CasFormat.cpp:102](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp:102)).

   **Smallest fix.** Reserve removal capacity through a CASed pool-wide admission object before appending, release the Pending slot on completion, and separately enforce a hard encoded-byte/count cap over permanent tombstones.

9. **Major — Persistent quarantine is underspecified at the safety boundary.**

   **Claim attacked.** Quarantine backoff avoids repeated reads while pool-wide destructive suppression remains safe.

   **Proof.** Existing durable coverage stores only classification, token, and cursor—no first-held round, retry generation, next probe, or offending anchor ([CasFoldSealFormat.h:32](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:32)). If a backoff round skips the held namespace and only current-round holds set `suppress_destructive`, destructive work resumes despite the unresolved possible `+1`. If carried classification 4 always suppresses, safety survives but one quarantined namespace freezes pool-wide deletion throughout its backoff; §12 must account for that liveness cost.

   **Smallest fix.** Persist quarantine metadata in the fold seal, define an explicit successful revalidation transition that clears it, and compute `suppress_destructive` from every carried quarantine on every round. Backoff may skip only its point reads.

10. **Major — N2 and §10 leave healthy sealed pools permanently `unchecked`.**

   **Claim attacked.** `NeverBorn` is consistent with codec invariants and fsck’s oracle.

   **Proof.** Empty recovery needs `sealed_from={0,0}` to express `(0,S]`, but the current snapshot codec rejects every zero `sealed_from` ([CasRefSnapshotFormat.cpp:69](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp:69)). V3 says the codec gains a state but does not define the required zero exception.

   Separately, fsck’s oracle assumes the newest snapshot ID is also a surviving log ID and skips otherwise ([CasFsck.cpp:220](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:220)). Every synthetic recovery seal—including `NeverBorn`—violates that assumption. Treating every such skip as whole-run `unchecked` means a healthy quiescent pool whose newest snapshot is a seal cannot return clean.

   **Smallest fix.** Define `NeverBorn` precisely: required zero `sealed_from`, no rows/remove ID, non-Live state, and greatest-applied equal to the seal ID. Add a seal-aware fsck path; absence of a same-ID log must not itself be interpreted as listing incompleteness.

11. **Minor — N8’s performance claim covers only repair GETs, not held-namespace cost.**

   **Claim attacked.** Quarantine backoff bounds the cost of held namespaces.

   **Proof.** Every fold still performs two full global ref-prefix enumerations ([CasGc.cpp:1013](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1013), [CasGc.cpp:2449](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2449)), materializes their key sets, groups every held namespace, and performs probe-A set comparisons. Permanent cursors/quarantines also enlarge every fold-seal encode/decode. Backoff bounds anchor and repair point reads only.

   The cleanup planner does receive the durable cursor, so that part of N4 is sound ([CasGc.cpp:2122](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2122)); however, it currently receives only snapshot IDs, not lifecycle metadata ([CasRefProtocol.cpp:691](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:691)). Identifying an old `Removed` snapshot outside the active cleanup-item set requires an extra snapshot/log read or new listing metadata.

   **Smallest fix.** Narrow §12 to “bounds additional chain-repair point reads,” account for the unavoidable two LISTs and comparisons, and specify how the planner identifies `Removed` snapshots.

N1’s interval construction survives conditionally: given an honest cold recovery listing, the seal key is derivable from namespace plus the `{E,MAX}` anchor, consecutive intervals are adjacent, and burned epochs create no uncovered record space. N4’s rebirth safety also survives because once the cursor equals the removal ID, the permanent tombstone is already durable; N5’s cut-scoped B1 identity is coherent provided every repaired/listed/void record uses the shared intake primitive. These do not repair O1.

**Overrule disposition:** O1 **DOES NOT SURVIVE**. O2 **DOES NOT SURVIVE AS WRITTEN**; a highest-allocated, correctly sampled, cross-epoch-initialized variant could be safe, but its pool-wide form still has indefinite namespace-level retention.

REJECT
tokens used
428,219
Both overrules fail under source-level counterexamples. O1 lacks a real completion-before-recovery ordering, and O2’s settlement premise is false in the current build lifecycle.

1. **Blocker — O1’s in-flight chronology is not a protocol guarantee.**

   **Claim attacked.** §2 says every ordinary same-node `PUT` settles before recovery lists the namespace.

   **Proof.** The favorable part is real: CAS selects a single-attempt S3 client, and the SDK retry strategy returns false, so no hidden retry layer resends after the controller stops running ([S3ObjectStorage.cpp:892](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:892), [Client.cpp:196](/home/mfilimonov/workspace/ClickHouse/master/src/IO/S3/Client.cpp:196)). But the controller’s five-second `attempt_timeout_ms` is explicitly only a scheduling check, not a socket deadline ([CasRequestControl.h:136](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h:136)); once admitted, it synchronously enters `backend->putIfAbsent` and cannot interrupt it when the fence later closes ([CasRequestControl.cpp:315](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:315)). The actual S3 send/receive timeout is independently configurable ([diskSettings.cpp:166](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/ObjectStorages/S3/diskSettings.cpp:166)), while self-remount waits only `attempt_timeout + margin` for lanes and then an independently configurable grace ([CasRefLedger.cpp:843](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:843), [CasPool.cpp:1018](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1018)).

   A request admitted just before fence expiry can therefore remain blocked through claim and grace, recovery can miss it, and the same already-started request can complete later. No connection-pool retry is needed. Process death likewise provides no source-backed bound on when a request already accepted by the server becomes visible.

   **Smallest fix.** Reinstate a point-readable per-namespace recovery authority. A timing-only alternative would require an enforced end-to-end request deadline plus a documented maximum server materialization delay and `grace` exceeding both; the present S3 contract supplies neither.

2. **Blocker — O1’s §5b detector is neither eventual nor preventive.**

   **Claim attacked.** A ghost folded under rule 1 will later be detected at an anchor crossing.

   **Proof.** Let the pre-recovery cursor be `A`, ghost log `V` have `prev=A`, and recovery seal `S` declare `(A,S]` void. If a hot fold lists `V` but no post-recovery successor, rule 1 folds `V`. Nothing references `S`, so no anchor crossing ever occurs if the namespace remains quiet. The cursor can authorize destructive work indefinitely without §5b running.

   If both `V` and successor `Y(prev=S)` appear in one sorted round, `V` is processed first. Rule 1 folds it; when `Y` later reveals `S`, §5b examines the round’s `parent_cursor=A`, not the now-resolved `V`, so its strict-inside test does not fire. Rule 3 also specifies only `resolved==sealed_from` and `resolved<sealed_from`; `A<resolved=V<S` is undefined. This contradicts the promised test that a ghost with `prev==cursor` is certified void ([spec:301](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:301)).

   **Smallest fix.** Discover the latest recovery authority before rule 1. Independently, make `sealed_from < resolved_through < seal_id` an immediate breach that clamps to the pre-interval cursor and keeps destructive suppression asserted. The latter does not solve the no-successor case by itself.

3. **Blocker — O2’s “grants settle before the floor passes” premise is false.**

   **Claim attacked.** Every owner grant below `min_active` has settled before the paired `R*`.

   **Proof.** `precommitAdd` marks its state `Uncertain` before entering `appendRefOps` ([CasPartWriteTxn.cpp:965](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:965)). If that append returns unresolved, the transaction can unwind while the grant may have landed. Its destructor nevertheless retires the build unconditionally ([CasPartWriteTxn.cpp:119](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:119)); `retireBuildSeq` immediately erases it from the active set ([CasMountRuntime.cpp:181](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp:181)). The next heartbeat can advance `min_active`.

   If “high-water” means settled/applied IDs, that unresolved grant may later become durable above `R*`. If it means highest allocated/attempted ID, the inequality can survive, but v3 never says that and its settlement proof remains wrong.

   **Smallest fix.** Define `R*` as the inclusive highest allocated/attempted ref ID, read `min_active` first and then the allocator high-water, and require grant ID allocation to happen-before build retirement. Alternatively, keep unresolved builds active until their owner duty is definitively settled.

4. **Blocker — O2 has no safe cross-epoch initialization.**

   **Claim attacked.** Recording `R*` at floor transitions covers every build already below the new mount’s floor.

   **Proof.** A fresh lease currently starts with `min_active=0` ([CasServerRoot.cpp:267](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:267)), while the sweep immediately considers every prior-epoch build eligible ([CasOrphanManifestSweep.cpp:263](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp:263)). The old lease and any old `R*` are overwritten. v3 does not specify what the new lease records before its first current-epoch build finishes.

   `{0,0}` as a present bound is unsafe because every cut satisfies it; “missing” is safe but disables reclamation. It is also not a valid real `RefTxnId` under the current ID contract ([CasTypes.h:265](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h:265)).

   **Smallest fix.** On epoch `E` claim, atomically publish either an explicitly absent bound that always retains, or `{E−1, UINT64_MAX}` covering every dead epoch. Define the first-epoch case and monotonic replacement rules explicitly.

5. **Major — A member-wide `R*` gives safety by retaining, but often never permits deletion.**

   **Claim attacked.** One pool-wide ref high-water is the identifier-space link the namespace sweep needs.

   **Proof.** Ref IDs are type-comparable, but namespace chains contain arbitrary global-counter gaps. If namespace `N` last appended ID 5, another namespace advances the member allocator to 1000 before the relevant build floor transition, then `R*=1000`. A quiet `N` remains chain-verified only through 5 forever, so every orphan manifest in `N` is retained until `N` writes again or a later remount seal dominates 1000. Recovery timing differences across namespaces produce the same stall.

   **Smallest fix.** Record a per-namespace grant high-water or a per-manifest grant certificate. Otherwise document that the proposed bound effectively disables sweep for quiet namespaces—the behavior O2 intended to overrule.

6. **Major — N7’s REBUILD quiescence proof is circular with today’s objects.**

   **Claim attacked.** REBUILD can prove every member lease expired or fenced before trusting its listings.

   **Proof.** Existing lease discovery itself uses `LIST(gc/server-roots/)` ([CasServerRoot.cpp:554](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:554)). Under v3’s hot-LIST trust boundary, an omitted live member makes “all members fenced” false. There is no fixed membership registry or pool-wide read-only fence. Meanwhile REBUILD discovers namespaces through another LIST ([CasGc.cpp:2393](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2393)) and condemns every physically listed blob absent from that reconstructed universe ([CasGc.cpp:2739](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2739)).

   **Smallest fix.** Require a durable pool-wide maintenance lease/fence checked by mount, renew, and mutation paths. Until it exists, specify that REBUILD always refuses—not that it may infer quiescence from lease enumeration.

7. **Major — N3’s sticky floor is stored in non-sticky state.**

   **Claim attacked.** The recovery requirement persists in the runtime across touches.

   **Proof.** Ref-table cache pressure erases entire runtimes ([CasRefLedger.cpp:829](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:829)), and self-remount marks every runtime superseded and clears the map ([CasRefLedger.cpp:929](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:929)). The next touch constructs a fresh runtime with no remembered floor. There is also a race during the unlocked recovery-seal `PUT`: remount can detach the runtime, after which recovery reacquires its mutex and installs without a final supersession check ([CasRefLedger.cpp:629](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:629), [CasRefLedger.cpp:665](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:665)).

   **Smallest fix.** Keep required floors in ledger-level state outside the evictable runtime, transfer them across remount, and reject recovery installation if `superseded_by_remount` became true.

8. **Major — N6’s admission cap has no linearizable admission authority and does not bound tombstones.**

   **Claim attacked.** A new `remove_namespace` is refused when the GC `Pending` set is at its cap.

   **Proof.** Writer admission currently recovers the namespace and immediately appends removal ([CasRefLedger.cpp:2890](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2890)). The `Pending` item does not exist until GC later folds that log ([CasGc.cpp:1689](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1689)). Multiple writers can therefore all observe a below-cap seal and append before any reservation appears. Moreover, limiting `Pending` does not limit permanent lineage tombstones, while the fold-seal format has a hard 256 MiB cap ([CasFormat.cpp:102](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp:102)).

   **Smallest fix.** Reserve removal capacity through a CASed pool-wide admission object before appending, release the Pending slot on completion, and separately enforce a hard encoded-byte/count cap over permanent tombstones.

9. **Major — Persistent quarantine is underspecified at the safety boundary.**

   **Claim attacked.** Quarantine backoff avoids repeated reads while pool-wide destructive suppression remains safe.

   **Proof.** Existing durable coverage stores only classification, token, and cursor—no first-held round, retry generation, next probe, or offending anchor ([CasFoldSealFormat.h:32](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:32)). If a backoff round skips the held namespace and only current-round holds set `suppress_destructive`, destructive work resumes despite the unresolved possible `+1`. If carried classification 4 always suppresses, safety survives but one quarantined namespace freezes pool-wide deletion throughout its backoff; §12 must account for that liveness cost.

   **Smallest fix.** Persist quarantine metadata in the fold seal, define an explicit successful revalidation transition that clears it, and compute `suppress_destructive` from every carried quarantine on every round. Backoff may skip only its point reads.

10. **Major — N2 and §10 leave healthy sealed pools permanently `unchecked`.**

   **Claim attacked.** `NeverBorn` is consistent with codec invariants and fsck’s oracle.

   **Proof.** Empty recovery needs `sealed_from={0,0}` to express `(0,S]`, but the current snapshot codec rejects every zero `sealed_from` ([CasRefSnapshotFormat.cpp:69](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp:69)). V3 says the codec gains a state but does not define the required zero exception.

   Separately, fsck’s oracle assumes the newest snapshot ID is also a surviving log ID and skips otherwise ([CasFsck.cpp:220](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:220)). Every synthetic recovery seal—including `NeverBorn`—violates that assumption. Treating every such skip as whole-run `unchecked` means a healthy quiescent pool whose newest snapshot is a seal cannot return clean.

   **Smallest fix.** Define `NeverBorn` precisely: required zero `sealed_from`, no rows/remove ID, non-Live state, and greatest-applied equal to the seal ID. Add a seal-aware fsck path; absence of a same-ID log must not itself be interpreted as listing incompleteness.
