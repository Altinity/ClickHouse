## Verdict

REVISE

## MAJOR findings

### The guarding `HEAD` has a TOCTOU hole when publication lands without marker reconciliation

**Exact spec section:** §1 “Change,” especially the soundness argument at lines 71–79, together with §1 “Not changed.”

The assertion that a failed publication’s “next condemn … writes its own marker” is false in the current implementation. `writeCondemnedMeta` treats any existing `Condemned` marker as success without writing or changing its etag ([`CasGcMetaWriter.cpp`, `writeCondemnedMeta`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp:70)).

Concrete data-loss interleaving:

1. Deposed leader A’s delayed job J reads old marker m1, then its proposed body `HEAD` observes absence.
2. Writer W also observes the body absent and publishes fresh incarnation t2. Publication can land while the call reports an ambiguous failure; in that case execution never reaches `reconcileMetaClean` ([`CasPartWriteTxn.cpp`, `ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:487)). The code explicitly retries or eventually throws after such ambiguity, while reconciliation occurs only after a successful return at line 523. Thus t2 can remain with unchanged marker m1.
3. W abandons its durable precommit, making t2 eligible for GC; `PartWriteTxn::abandon` durably removes that owner ([`CasPartWriteTxn.cpp`, `abandon`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:1109)).
4. Leader B condemns t2. Its marker job reads m1, sees it is already `Condemned`, returns `true` without a CAS, and records m1 as durable evidence for `(ref,t2)` ([`CasGcMetaWriter.cpp`, `writeCondemnedMeta` and `scheduleCondemnMarkerWrite`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp:73)).
5. J resumes and deletes m1 using its captured etag. There is no `Mismatch`: neither the failed writer nor B changed the marker. J’s body-absence observation is now stale, and t2 is present.
6. B’s later round accepts its in-process confirmation and graduates t2 ([`CasGc.cpp`, `confirm_condemned_marker`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1927); [`CasBlobInDegree.cpp`, `settleEntry`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:485)).
7. After B’s deleting-round cut, another transaction finds t2 present with no marker, backfills `Clean`, and adopts t2 without republishing ([`CasPartWriteTxn.cpp`, `ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:402)). B then deletes t2 by its exact token ([`CasGc.cpp`, pending-deletes loop](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:727)).

This execution satisfies the proposed implementation and its rewritten acceptance criterion: J observed absence in the same job and deleted exactly the etag it read. Therefore that criterion is insufficient.

Round-1 closure accounting:

- **Round-1 MAJOR 1 is not closed or moot.** Rev.2 removed marker claiming, but the ability to borrow an older `Condemned` marker is precisely what defeats the new `HEAD` proof.
- **Round-1 MAJOR 2 is moot.** The deletion no longer compares `condemn_round`, so rebuild round reuse is irrelevant to the proposed algorithm.
- **Round-1 MAJOR 3 is moot.** The unreachable TLA+ extension was dropped. Rev.2’s new problem is the unmodeled failed-publication interleaving above.

## MINOR findings

### The stated invariants are not equivalent

**Exact spec section:** §0 “Problem,” lines 36–38.

“Keep the condemn marker until incarnation X is deleted” does not imply “remove a marker only while no body exists at the key”: after X is deleted, a fresh incarnation Y may occupy the same key. The latter is the needed invariant, and “while” must mean at the marker-delete linearization point—not at an earlier `HEAD`.

### The tests omit the only transition that defeats the design

**Exact spec section:** §3, T1–T3.

T1 tests a body already present before J’s `HEAD`; T3 tests a marker whose etag changes before removal. Neither covers:

`J HEAD absent → body PUT lands → reconciliation does not land → condemner borrows unchanged Condemned marker → J removes it`.

A regression test must pause J after its absent body `HEAD`, inject a landed-but-reported-failed publication, and verify that a later condemnation cannot be confirmed using the unchanged marker.

The proposed remove-only latch correctly resolves round 1’s latch deadlock concern.

### The acceptance criterion and priority downgrade are premature

**Exact spec section:** §3, acceptance criterion 2 and the priority change.

“Observed absence earlier in the same job” is an implementation pattern, not a safety condition. The counterexample passes it. `CAS-30` should remain data-loss/critical until the marker-delete and marker-confirmation protocols jointly establish a happens-before edge.

## Answers to the spec's §5 review questions

1. **No.** The missing interleaving is:

   1. J reads m1.
   2. J observes the body absent.
   3. W’s body PUT lands, but W never lands `Clean`.
   4. B observes m1 already `Condemned` and confirms t2 without changing m1.
   5. J deletes m1 successfully.
   6. B graduates t2 using its recorded confirmation.
   7. A post-cut adopter observes absent metadata and adopts t2.
   8. B’s exact-token delete removes t2.

   HEAD-before-PUT orders W’s body publication after J’s observation, but there is no required marker mutation between that PUT and J’s delete. Therefore no happens-before edge closes the race.

2. **Skipping an extension of the existing model is defensible; skipping all model work is not.** The current model has one writer lifecycle and makes body deletion plus marker removal atomic in `GSettleRedelete` ([`CaGcCondemnMarkerGate.tla`](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaGcCondemnMarkerGate.tla:206)), so extending it would be awkward.

   A small focused model now pays for itself. It needs only body presence/token, marker etag/state, J’s three phases, landed-success/landed-failure publication, marker reconciliation, condemn confirmation, edge drop, and post-cut adoption. The direct proof already missed one of those branches.

3. **Only “never delete the marker” is simpler, and it has the acknowledged unbounded-key leak.** With separate body and marker keys, no finite sequence of `HEAD` followed by conditional marker deletion proves that the body is still absent at deletion time. One guarding `HEAD` is necessary but not sufficient under the current marker-confirmation rules.

## Alternatives

I recommend retaining rev.2’s asynchronous body `HEAD`, but coupling it with a real marker claim before a fresh incarnation may graduate:

1. `deleteConfirmedMeta` performs the proposed `loadMeta → body HEAD → exact marker delete`.
2. `writeCondemnedMeta` must not confirm an already-`Condemned` marker without changing its etag.
3. When the loaded marker is already `Condemned`, asynchronously CAS it to byte-distinct `Condemned` contents using the existing `condemn_round` field. Confirm only a committed create/CAS.
4. If the desired bytes equal the existing bytes—for example after round reuse—carry the entry and retry in a later round with that later round number. Do not treat equality as confirmation.
5. Remove the graduation gate’s “any durable `Condemned` marker is sufficient” re-read. Without an in-process successful claim, schedule an asynchronous claim and carry the entry.

The backend contract guarantees changed bytes produce a new incarnation value ([`CasBackend.h`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h:120)). Consequently, every condemnation after a fresh body publication either invalidates J’s etag or remains unconfirmed. If J deletes first, the condemner creates a fresh marker; if the condemner claims first, J gets `Mismatch`.

This needs no fields in `BlobMeta`, `RetiredEntry`, or `gc/state`, no format version, no writer-protocol change, and no synchronous wait for an individual meta operation. I recommend it over rev.2.
128,252
## Verdict

REVISE

## MAJOR findings

### The guarding `HEAD` has a TOCTOU hole when publication lands without marker reconciliation

**Exact spec section:** §1 “Change,” especially the soundness argument at lines 71–79, together with §1 “Not changed.”

The assertion that a failed publication’s “next condemn … writes its own marker” is false in the current implementation. `writeCondemnedMeta` treats any existing `Condemned` marker as success without writing or changing its etag ([`CasGcMetaWriter.cpp`, `writeCondemnedMeta`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp:70)).

Concrete data-loss interleaving:

1. Deposed leader A’s delayed job J reads old marker m1, then its proposed body `HEAD` observes absence.
2. Writer W also observes the body absent and publishes fresh incarnation t2. Publication can land while the call reports an ambiguous failure; in that case execution never reaches `reconcileMetaClean` ([`CasPartWriteTxn.cpp`, `ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:487)). The code explicitly retries or eventually throws after such ambiguity, while reconciliation occurs only after a successful return at line 523. Thus t2 can remain with unchanged marker m1.
3. W abandons its durable precommit, making t2 eligible for GC; `PartWriteTxn::abandon` durably removes that owner ([`CasPartWriteTxn.cpp`, `abandon`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:1109)).
4. Leader B condemns t2. Its marker job reads m1, sees it is already `Condemned`, returns `true` without a CAS, and records m1 as durable evidence for `(ref,t2)` ([`CasGcMetaWriter.cpp`, `writeCondemnedMeta` and `scheduleCondemnMarkerWrite`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp:73)).
5. J resumes and deletes m1 using its captured etag. There is no `Mismatch`: neither the failed writer nor B changed the marker. J’s body-absence observation is now stale, and t2 is present.
6. B’s later round accepts its in-process confirmation and graduates t2 ([`CasGc.cpp`, `confirm_condemned_marker`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1927); [`CasBlobInDegree.cpp`, `settleEntry`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:485)).
7. After B’s deleting-round cut, another transaction finds t2 present with no marker, backfills `Clean`, and adopts t2 without republishing ([`CasPartWriteTxn.cpp`, `ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:402)). B then deletes t2 by its exact token ([`CasGc.cpp`, pending-deletes loop](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:727)).

This execution satisfies the proposed implementation and its rewritten acceptance criterion: J observed absence in the same job and deleted exactly the etag it read. Therefore that criterion is insufficient.

Round-1 closure accounting:

- **Round-1 MAJOR 1 is not closed or moot.** Rev.2 removed marker claiming, but the ability to borrow an older `Condemned` marker is precisely what defeats the new `HEAD` proof.
- **Round-1 MAJOR 2 is moot.** The deletion no longer compares `condemn_round`, so rebuild round reuse is irrelevant to the proposed algorithm.
- **Round-1 MAJOR 3 is moot.** The unreachable TLA+ extension was dropped. Rev.2’s new problem is the unmodeled failed-publication interleaving above.

## MINOR findings

### The stated invariants are not equivalent

**Exact spec section:** §0 “Problem,” lines 36–38.

“Keep the condemn marker until incarnation X is deleted” does not imply “remove a marker only while no body exists at the key”: after X is deleted, a fresh incarnation Y may occupy the same key. The latter is the needed invariant, and “while” must mean at the marker-delete linearization point—not at an earlier `HEAD`.

### The tests omit the only transition that defeats the design

**Exact spec section:** §3, T1–T3.

T1 tests a body already present before J’s `HEAD`; T3 tests a marker whose etag changes before removal. Neither covers:

`J HEAD absent → body PUT lands → reconciliation does not land → condemner borrows unchanged Condemned marker → J removes it`.

A regression test must pause J after its absent body `HEAD`, inject a landed-but-reported-failed publication, and verify that a later condemnation cannot be confirmed using the unchanged marker.

The proposed remove-only latch correctly resolves round 1’s latch deadlock concern.

### The acceptance criterion and priority downgrade are premature

**Exact spec section:** §3, acceptance criterion 2 and the priority change.

“Observed absence earlier in the same job” is an implementation pattern, not a safety condition. The counterexample passes it. `CAS-30` should remain data-loss/critical until the marker-delete and marker-confirmation protocols jointly establish a happens-before edge.

## Answers to the spec's §5 review questions

1. **No.** The missing interleaving is:

   1. J reads m1.
   2. J observes the body absent.
   3. W’s body PUT lands, but W never lands `Clean`.
   4. B observes m1 already `Condemned` and confirms t2 without changing m1.
   5. J deletes m1 successfully.
   6. B graduates t2 using its recorded confirmation.
   7. A post-cut adopter observes absent metadata and adopts t2.
   8. B’s exact-token delete removes t2.

   HEAD-before-PUT orders W’s body publication after J’s observation, but there is no required marker mutation between that PUT and J’s delete. Therefore no happens-before edge closes the race.

2. **Skipping an extension of the existing model is defensible; skipping all model work is not.** The current model has one writer lifecycle and makes body deletion plus marker removal atomic in `GSettleRedelete` ([`CaGcCondemnMarkerGate.tla`](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaGcCondemnMarkerGate.tla:206)), so extending it would be awkward.

   A small focused model now pays for itself. It needs only body presence/token, marker etag/state, J’s three phases, landed-success/landed-failure publication, marker reconciliation, condemn confirmation, edge drop, and post-cut adoption. The direct proof already missed one of those branches.

3. **Only “never delete the marker” is simpler, and it has the acknowledged unbounded-key leak.** With separate body and marker keys, no finite sequence of `HEAD` followed by conditional marker deletion proves that the body is still absent at deletion time. One guarding `HEAD` is necessary but not sufficient under the current marker-confirmation rules.

## Alternatives

I recommend retaining rev.2’s asynchronous body `HEAD`, but coupling it with a real marker claim before a fresh incarnation may graduate:

1. `deleteConfirmedMeta` performs the proposed `loadMeta → body HEAD → exact marker delete`.
2. `writeCondemnedMeta` must not confirm an already-`Condemned` marker without changing its etag.
3. When the loaded marker is already `Condemned`, asynchronously CAS it to byte-distinct `Condemned` contents using the existing `condemn_round` field. Confirm only a committed create/CAS.
4. If the desired bytes equal the existing bytes—for example after round reuse—carry the entry and retry in a later round with that later round number. Do not treat equality as confirmation.
5. Remove the graduation gate’s “any durable `Condemned` marker is sufficient” re-read. Without an in-process successful claim, schedule an asynchronous claim and carry the entry.

The backend contract guarantees changed bytes produce a new incarnation value ([`CasBackend.h`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h:120)). Consequently, every condemnation after a fresh body publication either invalidates J’s etag or remains unconfirmed. If J deletes first, the condemner creates a fresh marker; if the condemner claims first, J gets `Mismatch`.

This needs no fields in `BlobMeta`, `RetiredEntry`, or `gc/state`, no format version, no writer-protocol change, and no synchronous wait for an individual meta operation. I recommend it over rev.2.
