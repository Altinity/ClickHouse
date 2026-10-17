## Verdict

REVISE

## MAJOR findings

### 1. The graduation gate can confirm an unclaimed older marker

**Exact spec section:** §1.B, especially “If the CAS conflicts, B's entry stays unconfirmed, the gate carries it,” together with §1 “Not changed.”

That statement is inconsistent with the implementation. `Gc::confirm_condemned_marker` accepts any durable `Condemned` marker without checking `condemn_round`; it then memoizes confirmation for the current `(ref, token)` ([`CasGc.cpp`, `confirm_condemned_marker`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1927)). `settleEntry` consequently graduates the entry ([`CasBlobInDegree.cpp`, `settleEntry`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:485)).

Concrete counterexample:

1. Body t1 was deleted and J, its `Condemned(R1)` cleanup, is delayed before `loadMeta`. The old marker remains.
2. A writer publishes t2 after observing the body absent. Publication happens before `reconcileMetaClean` ([`CasPartWriteTxn.cpp`, `ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:495)). If reconciliation fails and the build is later abandoned, t2 can remain garbage with the old `Condemned(R1)` marker.
3. B condemns t2 in R2. Its new “claim older marker” CAS fails or throws, leaving `Condemned(R1)`. Per-hash errors are swallowed by `GcMetaWriter::submit`, so the round can still commit the unconfirmed entry ([`CasGcMetaWriter.cpp`, `submit`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp:105)).
4. In the following round, `confirm_condemned_marker` sees `state == Condemned`, ignores `R1 != R2`, and graduates t2.
5. J resumes. Proposed change A sees its own `R1`, deletes the marker, and recreates the original loss window.
6. A writer landing after B’s fold cut adopts t2 through the absent-marker branch; the following round deletes t2 by its exact token.

Thus change B is not a graduation precondition. If A+B is retained, an out-of-process confirmation must reject an older marker and carry/retry the entry. The TLA+ model and tests also need the failed-claim branch.

### 2. Equal-round discrimination has no epoch boundary across a rebuilding reset

**Exact spec section:** §1.A “Why `condemn_round` identifies the condemn” and §4 “Round numbering after a rebuild.”

The spec records a data-loss trace as a residual but still treats the design as the fix. That is unacceptable for this bug class.

`Gc::rebuildBaseline` chooses the replacement round as `max(max_fence_round, state.round, max_gen) + 1`, where `max_gen` comes from enumerated `gc/gen` keys; it does not include any surviving `BlobMeta::condemn_round` ([`CasGc.cpp`, `rebuildBaseline`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:4277), [round selection](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:4522)). Consequently, after loss of `gc/state` and sufficiently recent generation artifacts—or an incomplete disaster-recovery enumeration—the new era can reuse an old marker round.

A stale cleanup job from a still-running deposed process can then match a new `Condemned(R)` marker numerically and delete it. There is no happens-before edge between the old pool job and the rebuilt counter era.

With complete surviving generation artifacts, the current round/generation progression likely keeps the rebuilt round above prior rounds. The spec must either prove that as a required rebuild invariant under every accepted recovery posture or avoid relying on round uniqueness. Calling the admitted collision merely residual leaves a reachable data-loss condition.

### 3. The proposed TLA+ extension cannot generate the claimed trace

**Exact spec section:** §2, particularly “the existing `WCreate` … followed by a second condemn.”

In the current model:

- `WPrecommit` changes `edge` from `"none"` to `"landed"`.
- `WCreate` requires `adopted = NoTok`, then sets it permanently to a token.
- No action drops the writer edge or resets `edge`, `adopted`, and `committed`.
- Every later `GCut` therefore sets `folded = TRUE`, preventing t2 from being condemned.

See [`WPrecommit`, `WCreate`, and `GCut`](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaGcCondemnMarkerGate.tla:104). The proposed `latedel_bug` trace is therefore unreachable as described.

The extension needs at least a writer/drop lifecycle permitting:

1. publication of t2;
2. removal of its edge so t2 becomes garbage;
3. condemnation and graduation of t2;
4. a distinct W2 edge landing after the deleting cut.

It must also model failed/swallowed marker claims and the permissive graduation re-read from major finding 1. The existing `GRestart` only clears in-process confirmation; it neither resets round numbering nor represents a delayed job from another leader.

## MINOR findings

### The latch described for T1b and T3 blocks the operations needed to advance the test

**Exact spec section:** §3, T1b and T3.

`MetaWriteLatchBackend` blocks every armed `.meta` write and removal ([`cas_test_helpers.h`, `MetaWriteLatchBackend::waitIfMeta`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/cas_test_helpers.h:2543)). Therefore:

- T1b’s separate marker replacement blocks on the same latch while the test is waiting to perform it before release.
- T3’s writer `Clean` reconciliation and B’s condemn write also block while A’s delete is held.

Use a one-shot, remove-only latch, or a hook that pauses a specifically identified request.

### A job does not survive a killed process or completed `Gc` destruction

**Exact spec section:** §0 and §4.

Every round installs `drainOnExitNoThrow`, and the successful path calls `drain` before committing `gc/state` ([`Gc::runRegularRound`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:410), [meta barrier](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:926)). The destruction test releases the job so destruction can join it ([`gtest_cas_gc_meta_writer.cpp`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_gc_meta_writer.cpp:28)).

The real hazard is a live but deposed process whose round remains blocked while another process advances—not a killed process executing a queued job afterward.

### The spec silently replaces the task’s acceptance criterion

**Exact spec section:** §3 “Acceptance criteria.”

The original task forbids deleting by an etag read after the deletion decision. The spec instead permits an execution-time read if state and round match. That may be a justified redesign, but the task text and acceptance criteria should be updated explicitly rather than “restated.”

## Answers to the spec's §5 review questions

1. **No.** In a healthy, unbroken counter lineage there is a useful ordering:

   `condemn t1 at R` → committed later-round graduation → committed later-round body deletion → publication of t2 → condemnation of t2 at `R' > R`.

   Under that premise, equality separates the markers.

   A rebuild breaks the premise:

   1. J for t1/R is queued after t1 becomes absent and remains blocked in a live deposed process.
   2. `gc/state` and sufficiently recent generation evidence are lost.
   3. `rebuildBaseline` selects a round below R because it does not inspect marker rounds.
   4. After enough rounds, t2 is condemned and graduated using numeric round R.
   5. J resumes, reads `Condemned(R)`, and deletes it.
   6. W2 adopts t2 after the graduation cut; the following exact-token delete removes t2.

   A genuinely killed process removes J and is harmless. A still-running deposed process is sufficient.

2. **B is necessary if A’s round-identity design is retained, but B as written is insufficient.** The borrowed-marker path is reachable, and an older marker must be claimed. Minimally, graduation must also require proof that the claim succeeded: an out-of-process re-check that sees `Condemned(old_round)` must carry the entry and schedule another asynchronous claim. Accepting any `Condemned` marker negates B.

3. **Yes.** Revalidate body absence in the asynchronous cleanup job; see the recommended alternative below. It removes the need for round identity and change B.

4. **Yes.** A reusable or second writer lifecycle is mandatory. The model needs an edge-drop/abandon action and a second precommit/adopt sequence. A two-leader restart action is not required for the minimal §0 late-delete trace, but it is required to validate claims about rebuild/reset and cross-leader jobs. Also model claim failure plus the production gate’s current “any `Condemned`” behavior.

## Alternatives

I recommend replacing A+B with an asynchronous body-absence revalidation:

1. `loadMeta` and retain its etag.
2. `HEAD blob/<hash>`.
3. If the body is present, skip cleanup.
4. If the body is absent, call `deleteMetaExact` with the etag from step 1.
5. Treat `Mismatch` as a safe no-op.

This directly enforces the invariant stated in §0: a marker is removable only while no body exists.

The race proof relies on the existing, untouchable HEAD-before-PUT protocol:

- If the body is present at step 2, the job deletes nothing.
- If no publication races, deleting the captured marker is safe.
- A publication after the absent HEAD must reconcile the marker to `Clean` after publishing ([`PartWriteTxn::ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:523)). It either changes the marker etag before the delete, causing `Mismatch`, or recreates/replaces `Clean` after the old marker is deleted.
- A failed publication may leave a fresh body with an absent marker, but no old exact-token delete can match that fresh incarnation.

This design:

- requires no persisted fields or format version;
- needs neither `condemn_round` discrimination nor marker claiming;
- remains entirely on the bounded asynchronous pool;
- adds one asynchronous body `HEAD` per confirmed cleanup;
- remains fail-closed when safety cannot be established.

If A+B is retained instead, it needs both strict round-aware graduation and a proven non-reuse rule across rebuilds. That is more state-machine surface and is not preferable to the body-absence check.
tokens used
132,670
## Verdict

REVISE

## MAJOR findings

### 1. The graduation gate can confirm an unclaimed older marker

**Exact spec section:** §1.B, especially “If the CAS conflicts, B's entry stays unconfirmed, the gate carries it,” together with §1 “Not changed.”

That statement is inconsistent with the implementation. `Gc::confirm_condemned_marker` accepts any durable `Condemned` marker without checking `condemn_round`; it then memoizes confirmation for the current `(ref, token)` ([`CasGc.cpp`, `confirm_condemned_marker`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1927)). `settleEntry` consequently graduates the entry ([`CasBlobInDegree.cpp`, `settleEntry`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:485)).

Concrete counterexample:

1. Body t1 was deleted and J, its `Condemned(R1)` cleanup, is delayed before `loadMeta`. The old marker remains.
2. A writer publishes t2 after observing the body absent. Publication happens before `reconcileMetaClean` ([`CasPartWriteTxn.cpp`, `ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:495)). If reconciliation fails and the build is later abandoned, t2 can remain garbage with the old `Condemned(R1)` marker.
3. B condemns t2 in R2. Its new “claim older marker” CAS fails or throws, leaving `Condemned(R1)`. Per-hash errors are swallowed by `GcMetaWriter::submit`, so the round can still commit the unconfirmed entry ([`CasGcMetaWriter.cpp`, `submit`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp:105)).
4. In the following round, `confirm_condemned_marker` sees `state == Condemned`, ignores `R1 != R2`, and graduates t2.
5. J resumes. Proposed change A sees its own `R1`, deletes the marker, and recreates the original loss window.
6. A writer landing after B’s fold cut adopts t2 through the absent-marker branch; the following round deletes t2 by its exact token.

Thus change B is not a graduation precondition. If A+B is retained, an out-of-process confirmation must reject an older marker and carry/retry the entry. The TLA+ model and tests also need the failed-claim branch.

### 2. Equal-round discrimination has no epoch boundary across a rebuilding reset

**Exact spec section:** §1.A “Why `condemn_round` identifies the condemn” and §4 “Round numbering after a rebuild.”

The spec records a data-loss trace as a residual but still treats the design as the fix. That is unacceptable for this bug class.

`Gc::rebuildBaseline` chooses the replacement round as `max(max_fence_round, state.round, max_gen) + 1`, where `max_gen` comes from enumerated `gc/gen` keys; it does not include any surviving `BlobMeta::condemn_round` ([`CasGc.cpp`, `rebuildBaseline`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:4277), [round selection](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:4522)). Consequently, after loss of `gc/state` and sufficiently recent generation artifacts—or an incomplete disaster-recovery enumeration—the new era can reuse an old marker round.

A stale cleanup job from a still-running deposed process can then match a new `Condemned(R)` marker numerically and delete it. There is no happens-before edge between the old pool job and the rebuilt counter era.

With complete surviving generation artifacts, the current round/generation progression likely keeps the rebuilt round above prior rounds. The spec must either prove that as a required rebuild invariant under every accepted recovery posture or avoid relying on round uniqueness. Calling the admitted collision merely residual leaves a reachable data-loss condition.

### 3. The proposed TLA+ extension cannot generate the claimed trace

**Exact spec section:** §2, particularly “the existing `WCreate` … followed by a second condemn.”

In the current model:

- `WPrecommit` changes `edge` from `"none"` to `"landed"`.
- `WCreate` requires `adopted = NoTok`, then sets it permanently to a token.
- No action drops the writer edge or resets `edge`, `adopted`, and `committed`.
- Every later `GCut` therefore sets `folded = TRUE`, preventing t2 from being condemned.

See [`WPrecommit`, `WCreate`, and `GCut`](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaGcCondemnMarkerGate.tla:104). The proposed `latedel_bug` trace is therefore unreachable as described.

The extension needs at least a writer/drop lifecycle permitting:

1. publication of t2;
2. removal of its edge so t2 becomes garbage;
3. condemnation and graduation of t2;
4. a distinct W2 edge landing after the deleting cut.

It must also model failed/swallowed marker claims and the permissive graduation re-read from major finding 1. The existing `GRestart` only clears in-process confirmation; it neither resets round numbering nor represents a delayed job from another leader.

## MINOR findings

### The latch described for T1b and T3 blocks the operations needed to advance the test

**Exact spec section:** §3, T1b and T3.

`MetaWriteLatchBackend` blocks every armed `.meta` write and removal ([`cas_test_helpers.h`, `MetaWriteLatchBackend::waitIfMeta`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/cas_test_helpers.h:2543)). Therefore:

- T1b’s separate marker replacement blocks on the same latch while the test is waiting to perform it before release.
- T3’s writer `Clean` reconciliation and B’s condemn write also block while A’s delete is held.

Use a one-shot, remove-only latch, or a hook that pauses a specifically identified request.

### A job does not survive a killed process or completed `Gc` destruction

**Exact spec section:** §0 and §4.

Every round installs `drainOnExitNoThrow`, and the successful path calls `drain` before committing `gc/state` ([`Gc::runRegularRound`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:410), [meta barrier](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:926)). The destruction test releases the job so destruction can join it ([`gtest_cas_gc_meta_writer.cpp`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests/gtest_cas_gc_meta_writer.cpp:28)).

The real hazard is a live but deposed process whose round remains blocked while another process advances—not a killed process executing a queued job afterward.

### The spec silently replaces the task’s acceptance criterion

**Exact spec section:** §3 “Acceptance criteria.”

The original task forbids deleting by an etag read after the deletion decision. The spec instead permits an execution-time read if state and round match. That may be a justified redesign, but the task text and acceptance criteria should be updated explicitly rather than “restated.”

## Answers to the spec's §5 review questions

1. **No.** In a healthy, unbroken counter lineage there is a useful ordering:

   `condemn t1 at R` → committed later-round graduation → committed later-round body deletion → publication of t2 → condemnation of t2 at `R' > R`.

   Under that premise, equality separates the markers.

   A rebuild breaks the premise:

   1. J for t1/R is queued after t1 becomes absent and remains blocked in a live deposed process.
   2. `gc/state` and sufficiently recent generation evidence are lost.
   3. `rebuildBaseline` selects a round below R because it does not inspect marker rounds.
   4. After enough rounds, t2 is condemned and graduated using numeric round R.
   5. J resumes, reads `Condemned(R)`, and deletes it.
   6. W2 adopts t2 after the graduation cut; the following exact-token delete removes t2.

   A genuinely killed process removes J and is harmless. A still-running deposed process is sufficient.

2. **B is necessary if A’s round-identity design is retained, but B as written is insufficient.** The borrowed-marker path is reachable, and an older marker must be claimed. Minimally, graduation must also require proof that the claim succeeded: an out-of-process re-check that sees `Condemned(old_round)` must carry the entry and schedule another asynchronous claim. Accepting any `Condemned` marker negates B.

3. **Yes.** Revalidate body absence in the asynchronous cleanup job; see the recommended alternative below. It removes the need for round identity and change B.

4. **Yes.** A reusable or second writer lifecycle is mandatory. The model needs an edge-drop/abandon action and a second precommit/adopt sequence. A two-leader restart action is not required for the minimal §0 late-delete trace, but it is required to validate claims about rebuild/reset and cross-leader jobs. Also model claim failure plus the production gate’s current “any `Condemned`” behavior.

## Alternatives

I recommend replacing A+B with an asynchronous body-absence revalidation:

1. `loadMeta` and retain its etag.
2. `HEAD blob/<hash>`.
3. If the body is present, skip cleanup.
4. If the body is absent, call `deleteMetaExact` with the etag from step 1.
5. Treat `Mismatch` as a safe no-op.

This directly enforces the invariant stated in §0: a marker is removable only while no body exists.

The race proof relies on the existing, untouchable HEAD-before-PUT protocol:

- If the body is present at step 2, the job deletes nothing.
- If no publication races, deleting the captured marker is safe.
- A publication after the absent HEAD must reconcile the marker to `Clean` after publishing ([`PartWriteTxn::ensureBlobPresent`](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:523)). It either changes the marker etag before the delete, causing `Mismatch`, or recreates/replaces `Clean` after the old marker is deleted.
- A failed publication may leave a fresh body with an absent marker, but no old exact-token delete can match that fresh incarnation.

This design:

- requires no persisted fields or format version;
- needs neither `condemn_round` discrimination nor marker claiming;
- remains entirely on the bounded asynchronous pool;
- adds one asynchronous body `HEAD` per confirmed cleanup;
- remains fail-closed when safety cannot be established.

If A+B is retained instead, it needs both strict round-aware graduation and a proven non-reuse rule across rebuilds. That is more state-machine surface and is not preferable to the body-absence check.
