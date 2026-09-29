---
description: 'Profile events and current metrics specific to the Antalya build.'
keywords: ['antalya', 'profile events', 'metrics', 'system.events', 'system.metrics']
sidebar_label: 'Profile Events and Metrics'
sidebar_position: 50
slug: /antalya/metrics-and-events
title: 'Antalya Profile Events and Metrics'
doc_type: 'reference'
---

# Antalya profile events and metrics {#antalya-profile-events-and-metrics}

This page lists the 277 profile events and 13 current metrics specific to this Antalya build.
Each row is the name and the `description` the server returns from
[`system.events`](/operations/system-tables/events) or
[`system.metrics`](/operations/system-tables/metrics). The text is the documentation string in
`src/Common/ProfileEvents.cpp` or `src/Common/CurrentMetrics.cpp`.

Profile events are cumulative counters. A counter that has never incremented is absent from
`system.events` unless the query sets `system_events_show_zero_values = 1`. A server restart resets
them. Per-query increments are in `system.query_log` (`ProfileEvents`), and the server-wide history
is in [`system.metric_log`](/operations/system-tables/metric_log) (`ProfileEvent_*` columns).

The type says how to read the counter: `Number` is a count, `Bytes` is a size, and `Microseconds`
or `Milliseconds` is accumulated time.

```sql
SELECT event, value, description
FROM system.events
WHERE event LIKE 'CAS%'
ORDER BY event
SETTINGS system_events_show_zero_values = 1
```

Current metrics are gauges. `system.metrics` always returns the current value, including zero.
History is in `system.metric_log` (`CurrentMetric_*` columns).

```sql
SELECT metric, value, description
FROM system.metrics
WHERE metric LIKE 'CAS%'
    OR metric IN (
        'Export',
        'IsSwarmModeEnabled',
        'PuffinFilesCacheBytes',
        'PuffinFilesCacheFiles')
ORDER BY metric
```

## Profile events {#profile-events}

### Content-addressed storage {#cas}

Counters for [content-addressed storage](/antalya/cas). For which of them to watch first, see
[CAS monitoring](/antalya/cas/operations/monitoring).

#### Lifecycle {#cas-lifecycle}

Pool lifecycle and teardown. A non-zero value here is an incident signal.

| Event | Type | Description |
| --- | --- | --- |
| `CASDataRootVanished` | `Number` | Counts CAS pools that entered a terminal Vanished lifecycle state (replaced / forgotten): the pool's data root was proven foreign (pool_id mismatch) or was decommissioned by SYSTEM CAS FORGET. The disk stays registered but store-class access fails loud with a typed error naming the sub-state; restart re-registers the name. |
| `CASDetachedWorkDrainTimeouts` | `Number` | Counts CAS storage teardowns whose bounded wait for detached background work expired with work still in flight. The teardown proceeds, but for that teardown it could not be established that no tracked task still holds the pool. Expected to stay at zero. |
| `CASEventDroppedContextExpired` | `Number` | Number of CAS system-log events dropped because the storage's `Context` reference expired before delivery. A non-zero value indicates event production outlived the owning server context. |
| `CASIdentityLost` | `Number` | Counts CAS pools that entered the IdentityLost lifecycle state: the pool sentinels (_pool_meta and the owner anchor) were observed authoritatively absent (both KeyAbsent). IdentityLost is a fail-loud TERMINAL state -- store-class access fails loud from this point and this pool's background threads (remount + GC) self-exit; there is no observer. Recover by restart or SYSTEM CAS FORGET; a matching-sentinel restore does not auto-revive. |

#### Blob requests {#cas-blob-requests}

These counters total object-store requests for keys under the blob prefix, across writers, GC,
validation, and other callers.

| Event | Type | Description |
| --- | --- | --- |
| `CASBlobAdoptTrusted` | `Number` | Number of CAS blob adoptions trusted through a durable manifest edge without per-file probes. Growth indicates manifest-based relinking. |
| `CASBlobBodyPutAvoided` | `Number` | Number of physical CAS blob body publications avoided after a safe presence observation found the object present. Growing values indicate saved upload traffic. |
| `CASBlobCompareSwap` | `Number` | Number of CAS blob compare-and-swap requests. Grows with concurrent metadata updates. |
| `CASBlobCompareSwapConflict` | `Number` | Number of CAS blob compare-and-swap conflicts. A non-zero value indicates concurrent update contention. |
| `CASBlobDelete` | `Number` | Number of CAS blob DELETE requests. Grows with cleanup and GC deletion activity. |
| `CASBlobGet` | `Number` | Number of CAS blob GET requests. Grows with blob reads and indicates read traffic. |
| `CASBlobGetStream` | `Number` | Number of streaming CAS blob GET requests. Grows with large or streamed blob reads. |
| `CASBlobHead` | `Number` | Number of successful backend HEAD requests under CAS blob paths that found an object, aggregated across all callers. |
| `CASBlobHeadMiss` | `Number` | Number of successful backend HEAD requests under CAS blob paths that found no object, aggregated across all callers. |
| `CASBlobList` | `Number` | Number of CAS blob LIST requests. Growing values indicate more object enumeration. |
| `CASBlobOverwrite` | `Number` | Number of CAS blob overwrite requests. Growing values indicate repeated replacement writes. |
| `CASBlobPut` | `Number` | Number of CAS blob PUT requests. Grows with blob uploads and indicates storage write traffic. |
| `CASBlobPutDeduplicated` | `Number` | Number of CAS blob deduplicating PUT requests. Grows when uploads reuse existing content. |

#### Blob upload fan-out {#cas-blob-upload}

These counters cover the intra-part fan-out of unique blob uploads.

| Event | Type | Description |
| --- | --- | --- |
| `CASBlobUploadFanoutBatches` | `Number` | Number of CAS intra-part blob-upload fan-out invocations (one per part publish that had at least one unique blob to upload). Compare with CASBlobUploadFanoutTasks for the average number of unique blobs uploaded per part. |
| `CASBlobUploadFanoutTasks` | `Number` | Number of CAS blob-upload tasks dispatched to the fan-out pool (one per unique blob ref across all part publishes). Duplicate references within a part collapse to one task. |

#### Bulk delete {#cas-bulk-delete}

This counter covers batch deletion of write-once keys.

| Event | Type | Description |
| --- | --- | --- |
| `CASBulkDeleteRequests` | `Number` | Number of CAS batch delete requests: one DeleteObjects carrying up to 1000 write-once keys (manifest bodies, ref logs, ref snapshots). The per-key class counters (CASManifestDelete, CASRootDelete) say how many keys each request carried. |

#### Manifest requests {#cas-manifest-requests}

These counters total object-store requests for manifest keys.

| Event | Type | Description |
| --- | --- | --- |
| `CASManifestCompareSwap` | `Number` | Number of CAS manifest compare-and-swap requests. Grows with concurrent manifest updates. |
| `CASManifestCompareSwapConflict` | `Number` | Number of CAS manifest compare-and-swap conflicts. A non-zero value indicates concurrent update contention. |
| `CASManifestDelete` | `Number` | Number of CAS manifest DELETE requests. Grows with cleanup and GC deletion activity. |
| `CASManifestGet` | `Number` | Number of CAS manifest GET requests. Grows with manifest reads and indicates metadata read traffic. |
| `CASManifestGetStream` | `Number` | Number of streaming CAS manifest GET requests. Grows with large or streamed manifest reads. |
| `CASManifestHead` | `Number` | Number of CAS manifest HEAD requests. Grows with manifest existence checks and validation. |
| `CASManifestHeadMiss` | `Number` | Number of CAS manifest HEAD requests that found no object. A non-zero value indicates missing or stale manifests. |
| `CASManifestList` | `Number` | Number of CAS manifest LIST requests. Growing values indicate more manifest enumeration. |
| `CASManifestOverwrite` | `Number` | Number of CAS manifest overwrite requests. Growing values indicate repeated manifest replacement. |
| `CASManifestPut` | `Number` | Number of CAS manifest PUT requests. Grows with manifest creation and indicates metadata write traffic. |
| `CASManifestPutDeduplicated` | `Number` | Number of CAS deduplicating manifest PUT requests. Grows when existing manifests are reused. |

#### Root requests {#cas-root-requests}

These counters total object-store requests for root keys.

| Event | Type | Description |
| --- | --- | --- |
| `CASRootCompareSwap` | `Number` | Number of CAS root compare-and-swap requests. Grows with concurrent root updates. |
| `CASRootCompareSwapConflict` | `Number` | Number of CAS root compare-and-swap conflicts. A non-zero value indicates concurrent update contention. |
| `CASRootDelete` | `Number` | Number of CAS root DELETE requests. Grows with cleanup activity. |
| `CASRootGet` | `Number` | Number of CAS root GET requests. Grows with root reads and indicates metadata read traffic. |
| `CASRootGetStream` | `Number` | Number of streaming CAS root GET requests. Grows with large or streamed root reads. |
| `CASRootHead` | `Number` | Number of CAS root HEAD requests. Grows with root existence checks and validation. |
| `CASRootHeadMiss` | `Number` | Number of CAS root HEAD requests that found no object. A non-zero value indicates missing or stale roots. |
| `CASRootList` | `Number` | Number of CAS root LIST requests. Growing values indicate more root enumeration. |
| `CASRootOverwrite` | `Number` | Number of CAS root overwrite requests. Growing values indicate repeated root replacement. |
| `CASRootPut` | `Number` | Number of CAS root PUT requests. Grows with root creation and indicates metadata write traffic. |
| `CASRootPutDeduplicated` | `Number` | Number of CAS deduplicating root PUT requests. Grows when existing roots are reused. |

#### Server-object requests {#cas-server-requests}

These counters total object-store requests for server-object keys.

| Event | Type | Description |
| --- | --- | --- |
| `CASServerCompareSwap` | `Number` | Number of CAS server-object compare-and-swap requests. Grows with server metadata updates. |
| `CASServerCompareSwapConflict` | `Number` | Number of CAS server-object compare-and-swap conflicts. A non-zero value indicates concurrent updates. |
| `CASServerDelete` | `Number` | Number of CAS server-object DELETE requests. Grows with server-state cleanup. |
| `CASServerGet` | `Number` | Number of CAS server-object GET requests. Grows with server metadata reads. |
| `CASServerGetStream` | `Number` | Number of streaming CAS server-object GET requests. Grows with large metadata reads. |
| `CASServerHead` | `Number` | Number of CAS server-object HEAD requests. Grows with existence checks and validation. |
| `CASServerHeadMiss` | `Number` | Number of CAS server-object HEAD requests that found no object. A non-zero value indicates missing server state. |
| `CASServerList` | `Number` | Number of CAS server-object LIST requests. Growing values indicate more server-state enumeration. |
| `CASServerOverwrite` | `Number` | Number of CAS server-object overwrite requests. Growing values indicate repeated replacement writes. |
| `CASServerPut` | `Number` | Number of CAS server-object PUT requests. Grows with server metadata writes. |
| `CASServerPutDeduplicated` | `Number` | Number of deduplicating CAS server-object PUT requests. Growth indicates reused server objects. |

#### Other-object requests {#cas-other-requests}

These counters total object-store requests for CAS keys outside the blob, manifest, root, and server-object prefixes.

| Event | Type | Description |
| --- | --- | --- |
| `CASOtherCompareSwap` | `Number` | Number of CAS other-object compare-and-swap requests. Grows with metadata updates. |
| `CASOtherCompareSwapConflict` | `Number` | Number of CAS other-object compare-and-swap conflicts. A non-zero value indicates concurrent updates. |
| `CASOtherDelete` | `Number` | Number of CAS other-object DELETE requests. Grows with cleanup activity. |
| `CASOtherGet` | `Number` | Number of CAS other-object GET requests. Grows with reads of uncategorized CAS objects. |
| `CASOtherGetStream` | `Number` | Number of streaming CAS other-object GET requests. Grows with large reads. |
| `CASOtherHead` | `Number` | Number of CAS other-object HEAD requests. Grows with existence checks and validation. |
| `CASOtherHeadMiss` | `Number` | Number of CAS other-object HEAD requests that found no object. A non-zero value indicates missing state. |
| `CASOtherList` | `Number` | Number of CAS other-object LIST requests. Growing values indicate more object enumeration. |
| `CASOtherOverwrite` | `Number` | Number of CAS other-object overwrite requests. Growing values indicate repeated replacement writes. |
| `CASOtherPut` | `Number` | Number of CAS other-object PUT requests. Grows with writes to uncategorized CAS objects. |
| `CASOtherPutDeduplicated` | `Number` | Number of deduplicating CAS other-object PUT requests. Growth indicates reused uncategorized objects. |

#### Garbage collection {#cas-gc}

These counters cover GC rounds, retirement, and the object-store requests GC itself issues.
Day-to-day interpretation is on the [monitoring](/antalya/cas/operations/monitoring) page.

| Event | Type | Description |
| --- | --- | --- |
| `CASGCClampSuppressedPasses` | `Number` | Number of CAS GC passes that deferred shard graduation and deletion because reachability was uncertain. In the current stage this is EVERY folding round by construction, not an anomaly: the namespace universe is not yet knowable, so the round's destructive gate is shut unconditionally and this counter tracks folding rounds rather than trouble. What does discriminate is the fold seal's hold set and the tables_held column of the fold_ref_intake phase row -- read those, not this, to tell a healthy suppressed round from a namespace that stopped. |
| `CASGCCompareSwap` | `Number` | Number of CAS GC compare-and-swap requests. Grows with GC state updates. |
| `CASGCCompareSwapConflict` | `Number` | Number of CAS GC compare-and-swap conflicts. A non-zero value indicates concurrent GC or metadata updates. |
| `CASGCCondemnMarkerUnconfirmedCarry` | `Number` | Number of CAS GC retirements delayed because a durable condemn marker could not be confirmed. A non-zero value indicates marker write or read failures and safely postpone deletion. |
| `CASGCDeadPrecommitSkipped` | `Number` | Number of CAS GC edges skipped for precommits proven dead below the durable watermark. Growing values indicate stale or incomplete ref history. |
| `CASGCDelete` | `Number` | Number of CAS GC DELETE requests. Grows with successful cleanup attempts. |
| `CASGCEnumerationPages` | `Number` | Number of CAS GC LIST pages fetched while enumerating the object universe. Growing values indicate a larger universe or more frequent scans. |
| `CASGCGet` | `Number` | Number of CAS GC GET requests. Grows with collection reads and recovery work. |
| `CASGCGetStream` | `Number` | Number of streaming CAS GC GET requests. Grows with large collection or recovery reads. |
| `CASGCHead` | `Number` | Number of CAS GC HEAD requests. Grows with object existence checks during collection. |
| `CASGCHeadMiss` | `Number` | Number of CAS GC HEAD requests that found no object. A non-zero value indicates already-removed or missing objects. |
| `CASGCHeartbeatFenceOuts` | `Number` | Number of CAS GC operations fenced out for expired mounts. A non-zero value indicates stale mounts or heartbeat delays. |
| `CASGCList` | `Number` | Number of CAS GC LIST requests. Growing values indicate more collection enumeration. |
| `CASGCMetaOps` | `Number` | Number of per-hash metadata operations executed by CAS GC. Growing values indicate more GC candidates or metadata work. |
| `CASGCMetaWriteAnomaly` | `Number` | Number of CAS GC metadata operations that failed on the bounded metadata pool. A non-zero value indicates backend or pool pressure and may delay metadata convergence. |
| `CASGCNamespaceCleanupLeaks` | `Number` | Number of dead-life namespace objects whose reclamation the perpetual janitor could not confirm because HEAD or exact-delete failed. Each occurrence is logged with the exact key and remains leak-only: it neither suppresses destructive GC nor blocks catalog lifecycle progress. |
| `CASGCOverwrite` | `Number` | Number of CAS GC overwrite requests. Growing values indicate repeated GC metadata replacement. |
| `CASGCPut` | `Number` | Number of CAS GC PUT requests. Grows with GC metadata and object-management writes. |
| `CASGCPutDeduplicated` | `Number` | Number of deduplicating CAS GC PUT requests. Growth indicates GC reused existing objects. |
| `CASGCReadAheadHit` | `Number` | Number of CAS GC fold reads and HEADs answered by the fold's read-ahead. Growth means the round's small-object round trips overlapped instead of serializing. |
| `CASGCReadAheadMiss` | `Number` | Number of CAS GC fold reads and HEADs performed inline because nothing was hinted for the key. A large value against hits means a hint set is narrower than the walk. |
| `CASGCReadAheadWasted` | `Number` | Number of CAS GC read-ahead results fetched and never taken: a namespace held below its lookahead, or a HEAD candidate that kept an edge. Bounded by the read-ahead window per namespace. |
| `CASGCRebuildVirginByEnumeration` | `Number` | Number of times SYSTEM CAS GC REBUILD concluded FROM ENUMERATION ALONE that a pool had never sealed a baseline, and so carried no durable holds forward. The verdict rests on three pieces of evidence -- a wide LIST of the gc/gen prefix empty, a narrow probe of gc/gen/1/ empty, and gc/state absent -- and on no point read, because the fold seal key needs an attempt component that is a lease sequence number and has no arithmetic successor to probe. An enumeration that hid every seal INCLUDING generation 1 on a lived-in pool would therefore read as virgin here. Expected to be 0 on any pool that has ever completed a GC round; a nonzero value on such a pool means the rebuild was granted a clean slate it could not prove. |
| `CASGCRefWalkPlansBuilt` | `Number` | Number of complete catalog-authoritative CAS ref walk plans constructed by ordinary GC and rebuild. A regular or rebuilding invocation that reaches the post-LIST catalog cut increments this exactly once, including a round that later defers. |
| `CASGCRetireReplaced` | `Number` | Number of CAS GC re-condemnations after a resurrected object replaced a retired incarnation. Growth indicates object-generation churn. |
| `CASGCRetiredCondemned` | `Number` | Number of CAS GC entries newly condemned into the retired set. Growth indicates objects becoming eligible for later cleanup. |
| `CASGCRetiredGraduated` | `Number` | Number of CAS GC retired entries that passed the safety floor and became pending deletion. Growth indicates cleanup progress. |
| `CASGCRetiredRedeleted` | `Number` | Number of CAS GC pending deletes executed with an exact object token. Growth indicates physical cleanup activity. |
| `CASGCRetiredSpared` | `Number` | Number of CAS GC retired entries spared after references returned. Growing values indicate reference churn or resurrection. |
| `CASGCRetiredSparedByReref` | `Number` | Number of CAS GC delete_pending entries spared because a fresh deduplicating adopt re-referenced them after graduation (an ordinary observe/condemn race, not a fail-closed abort). Subset of CASGCRetiredSpared. |
| `CASGCStuckRemovals` | `Number` | Number of adopted CAS GC rounds that observed a Removing namespace at or beyond the diagnostic age threshold without terminal cleanup evidence. Incremented and warned every such round; diagnostic only, with no effect on folding, suppression, appends, or deletion. |
| `CASGCUnappliedFoldedTransactions` | `Number` | Number of ref transactions a GC round folded and merged but whose blob deltas never reached a shard reducer. Always 0 on a healthy round; a nonzero value fails the round closed, because the round would otherwise advance its fold cursor past a transaction it never applied. |
| `CASGCUnmatchedAdoptedParentLives` | `Number` | Number of adopted-parent CAS ref-life rows dropped because the post-LIST catalog cut has no matching physical life. Each occurrence is inert for planning and suppression and is logged with its exact physical life id; a persistent nonzero rate indicates old generation state is outliving catalog removal. |
| `CASGCUnmatchedRemoveDeltas` | `Number` | Number of CAS GC in-degree removal deltas that matched no existing source edge. The in-degree model is a set, not a counter, so this is a per-key no-op by design and never causes a false deletion — but a persistent nonzero rate means removal deltas are reaching the reducer without their matching activation, which is a correctness signal. |

#### References {#cas-references}

These counters cover ref-log appends, recovery, snapshots, and reachability edges.

| Event | Type | Description |
| --- | --- | --- |
| `CASRefAppendDefiniteFailure` | `Number` | Number of CAS ref-log appends rejected with certainty. A non-zero value indicates invalid requests or backend rejection requiring investigation. |
| `CASRefAppendOccupantUnreadable` | `Number` | Number of CAS ref-log appends that met a DIFFERENT object at the id they derived and could not read it to tell a successor's epoch seal from a breach of mount write-exclusivity. The decision is deferred to the next attempt, which re-derives the same id. Sustained growth means a real breach may be going unreported: the loud interference path is only reached once the occupant can be read. |
| `CASRefAppendPreAttemptRefused` | `Number` | Number of CAS ref-log append chunks refused BEFORE any request was sent (the mount fence or the operation deadline rejected while zero attempts had been made). Nothing can be durable, so the lane is deliberately NOT wedged and the caller retries. Counted separately from `CASRefAppendWedged` so a falling wedge count can be read as the availability fix working rather than as nothing happening. |
| `CASRefAppendSealRejected` | `Number` | Number of CAS ref-log transactions conclusively rejected by a successor's epoch seal occupying the id they derived. This is the protocol working -- the writer was deposed and its operation was never acknowledged -- but a lane that keeps counting here is a writer that has lost its mount and does not yet know it. |
| `CASRefAppendUnwedged` | `Number` | Number of CAS ref-log append lanes recovered after an uncertain PUT was later observed durable. A non-zero value indicates transient write uncertainty. |
| `CASRefAppendWedged` | `Number` | Number of CAS ref-log append lanes that exhausted retries after an uncertain PUT. A non-zero value indicates ref-log progress may be stalled. |
| `CASRefBatchFlushes` | `Number` | Number of committed CAS ref-log batch flushes. Compare with CASRefBatchedMutations to assess batching efficiency. |
| `CASRefBatchScopeCuts` | `Number` | Number of CAS ref batches cut short by scope limits. Growing values indicate smaller batches and more write overhead. |
| `CASRefBatchedMutations` | `Number` | Number of CAS ref mutations committed through the per-namespace batching queue. Growth indicates reference-write activity. |
| `CASRefCheckpointIdenticalSkip` | `Number` | Number of CAS ref `_ckpt` updates that needed no write because the merged body already matched the stored one. A high ratio against CASRefCheckpointPublished is the intended steady state. |
| `CASRefCheckpointNotAdvanced` | `Number` | Number of CAS ref-table snapshot publications whose `_ckpt` checkpoint could not be advanced (fenced out, contended, or failed). The snapshot body is durable but is not yet cleanup-authoritative; a non-zero value means recovery keeps replaying from an older base. |
| `CASRefCheckpointPublished` | `Number` | Number of CAS ref `_ckpt` checkpoint objects durably updated by a token compare-and-swap. Growth tracks snapshot publication and namespace creation. |
| `CASRefCleanupObjectsDeleted` | `Number` | Number of old CAS ref logs and snapshots deleted after safe coverage was confirmed. Includes keys that were already absent, since a batch delete of write-once keys cannot tell the two apart. Growth indicates cleanup progress. |
| `CASRefEmittedEdges` | `Number` | Number of reachability edges emitted while GC folds CAS reference history. Growth indicates more reference relationships to process. |
| `CASRefGlobalListPages` | `Number` | Number of CAS ref LIST pages fetched during GC. Growing values indicate more refs or smaller backend pages to scan. |
| `CASRefLogBodyGets` | `Number` | Number of CAS ref-log bodies read and decoded during GC. Growth indicates more reference history to process. |
| `CASRefManifestBodyFoldGets` | `Number` | Number of manifest bodies read while GC follows reference edges. High values indicate cache misses or many referenced manifests. |
| `CASRefMaterializeCopy` | `Number` | Number of CAS ref-table COW container materializations that had to build a fresh base because a copy still shared it (the O(table size) slow path). A persistently high value versus CASRefMaterializeInPlace means scratch copies are outliving the state-install point. |
| `CASRefMaterializeInPlace` | `Number` | Number of CAS ref-table COW container materializations that folded the overlay into a uniquely-owned base in place (the production flush fast path, O(overlay)). Compare with CASRefMaterializeCopy: a high ratio confirms the fast path is firing in production. |
| `CASRefNeedsRecovery` | `Number` | Number of CAS ref append lanes moved to `NeedsRecovery` because a known-durable transaction could not be installed. Such a lane refuses writes, snapshots, and confirmation until durable replay completes. |
| `CASRefQueueWaitMicroseconds` | `Microseconds` | Total time CAS ref writers spent queued, in microseconds. A rising value indicates ref-write contention or backend latency. |
| `CASRefRecoveryCancelled` | `Number` | Number of CAS ref-table recovery attempts abandoned because a self-remount requested cancellation before re-arming the mount fence. Non-zero means remounts are overlapping recoveries; nothing is written or installed on this path. |
| `CASRefRecoveryEpochSealAdopted` | `Number` | Number of dead CAS ref writer epochs a recovery compare-and-swap walk found ALREADY closed by a concurrent recoverer's seal and adopted. A peer that got there first is the designed outcome, not contention to alarm on. |
| `CASRefRecoveryEpochSealed` | `Number` | Number of CAS ref epoch-seal transactions MINTED by a recovery compare-and-swap walk, one per dead writer epoch it closed. Each seal occupies the exact log key a dying predecessor's in-flight PUT would take, so growth tracks epoch transitions over touched namespaces (including burned, never-written epochs), not anomalies. |
| `CASRefRecoveryRestarts` | `Number` | Number of CAS ref-table recovery retries after a snapshot or log vanished during reading. A non-zero value indicates concurrent cleanup or backend inconsistency. |
| `CASRefRecoveryRetries` | `Number` | Number of CAS ref-table recovery attempts retried after a transient object-store error before the table's load fails. A non-zero value indicates transient object-store disruption during table startup. |
| `CASRefRecoveryStragglerAdopted` | `Number` | Number of straggler ref-log transactions a recovery compare-and-swap walk met at the slot it tried to seal and adopted, re-sealing at the new T+1. Non-zero means writes from a dying epoch were still materializing when recovery ran. |
| `CASRefRecoveryStreamHole` | `Number` | Number of times CAS ref-table recovery found a 404 BELOW a durable same-epoch witness -- a hole in a stream INV-1 makes dense. Restarted while the restart budget lasts (a racing cleanup is the innocent explanation), then reported as corruption. Any sustained non-zero value is data loss, not noise. |
| `CASRefRepoint` | `Number` | Number of CAS committed-reference repoints that republish a manifest. Growth indicates standalone part updates or removals. |
| `CASRefRollbackBestEffortDropFailed` | `Number` | Number of rollback cleanup drops that hit a backend failure. A non-zero value means refs may remain live and GC may be delayed. |
| `CASRefSnapshotPublishBackoff` | `Number` | Number of CAS ref-table snapshot publications put into backoff after a non-committed result. A non-zero value indicates write failure or uncertainty. |
| `CASRefSnapshotPublishDispatched` | `Number` | Number of background CAS ref-table snapshot publications started. High values indicate frequent threshold or read-triggered publishing. |
| `CASRefSnapshotPutBytes` | `Bytes` | Total bytes written to CAS ref-table snapshots. A high value indicates frequent or large snapshot publication. |
| `CASRefSnapshotTailLogs` | `Number` | Number of CAS ref-log entries compacted into published snapshots. Growth indicates snapshot maintenance work. |
| `CASRefStalePrecommitsReclaimed` | `Number` | Number of stale precommit bindings removed by CAS ref cleanup. Growth indicates superseded writer state is being reclaimed. |
| `CASRefSweepDeferred` | `Number` | Number of stale-precommit sweeps deferred after a read-only failure. A non-zero value indicates cleanup is waiting for a later trigger. |
| `CASRefSweepRearmed` | `Number` | Number of failed or partial stale-precommit sweeps scheduled for retry. Growing values indicate persistent cleanup or backend errors. |
| `CASRefTableEvictions` | `Number` | Number of CAS ref-cache table evictions caused by the memory budget. Growing values indicate a small cache and more recovery I/O. |

#### Blob metadata {#cas-blob-metadata}

These counters cover per-blob metadata creates, rewrites, and deletes.

| Event | Type | Description |
| --- | --- | --- |
| `CASMetaAdoptBackfill` | `Number` | Number of blob-adoption paths that attempted a missing-metadata Clean backfill. Counts the attempt, not a guaranteed metadata creation. |
| `CASMetaCompareSwap` | `Number` | Number of conditional CAS blob-metadata rewrites, including lost races. Growing values indicate metadata updates or contention. |
| `CASMetaCreateClean` | `Number` | Number of absent-body publication paths that entered Clean metadata reconciliation. Counts the reason entry, not a guaranteed metadata creation. |
| `CASMetaDelete` | `Number` | Number of exact-token CAS blob-metadata delete attempts. Growth indicates GC or cleanup activity. |
| `CASMetaPut` | `Number` | Number of conditional CAS blob-metadata create attempts, including lost races. Growth indicates metadata reconciliation or write contention. |
| `CASMetaResurrectClean` | `Number` | Number of condemned-body replacement paths that entered Clean metadata reconciliation. Counts the reason entry, not a guaranteed metadata reset. |

#### Mounts {#cas-mounts}

These counters cover mount-lease renewal. They are process-global, so read the change over a
window.

| Event | Type | Description |
| --- | --- | --- |
| `CASMountExclusivityViolation` | `Number` | Counts CAS mount releases where a runtime that still BELIEVED IT OWNED the mount (no deposition ever observed) found a FOREIGN occupant in the slot. This is the single-writer guarantee being broken, not a failover: the release refuses, the slot is left untouched, and the write fence is latched so the runtime stops trusting itself. This must always be zero. |
| `CASMountLeaseLost` | `Number` | Counts exactly once per operational CAS mount-lease Live-to-TransientNotLive loss/recovery generation. The initiating external loss or the first ordinary terminal renewal consumer owns the increment, including external lease-safety deadline exhaustion; parked/classification/shutdown paths do not duplicate it. |
| `CASMountReleaseSkippedForeignOccupant` | `Number` | Counts conclusive CAS mount-renewal observations that found a FOREIGN successor in the slot. This is the EXPECTED end state of a failover: renewal fences the deposed runtime, terminal teardown skips the farewell without backend I/O, and the successor's slot is left byte-for-byte untouched. Steady non-zero values on a cluster that is not failing over are worth investigating; a value that tracks failovers is normal. |
| `CASMountRenewalAttempts` | `Number` | Number of physical conditional renewal PUTs sent for CAS mount leases. This counts transport attempts, not logical renewals. |
| `CASMountRenewalDeadlineExceeded` | `Number` | Number of logical CAS mount-lease renewals stopped by the external lease-safety deadline. Growth means the last confirmed lease no longer had enough safe time for another physical attempt. |
| `CASMountRenewalRecovered` | `Number` | Number of logical CAS mount-lease renewals that committed after a physical retry or exact resolving GET. |
| `CASMountRenewalResolved` | `Number` | Number of CAS mount-lease renewals whose committed outcome was proved by an exact resolving GET. |
| `CASMountRenewalRetries` | `Number` | Number of physical conditional renewal PUTs sent after the first attempt of one logical CAS mount-lease renewal. |

#### Remount {#cas-remount}

These counters cover whole-chain remount attempts after a mount leaves `Live`.

| Event | Type | Description |
| --- | --- | --- |
| `CASRemountAttempts` | `Number` | Number of invocations of the CAS whole-chain remount attempt. |
| `CASRemountFailed` | `Number` | Number of CAS whole-chain remount attempts that returned without restoring Live, including caught step exceptions. |
| `CASRemountHeldTransient` | `Number` | Counts CAS remount attempts that could not decide the pool's identity and were held transient: the `_pool_meta` probe was inconclusive or its body could not be decoded (a partially written object, an unreadable store, or a pool whose format this build no longer reads). The mount stays fenced closed and the remount loop retries; a value that keeps growing means the pool will never remount without operator action -- read the accompanying warning for the probe's own reason. |
| `CASRemountSucceeded` | `Number` | Number of CAS whole-chain remount attempts that restored Live under a fresh writer epoch. |

#### Request contract {#cas-request-contract}

These counters cover the shared CAS request engine: attempts, reissues, refusals, and unresolved writes.

| Event | Type | Description |
| --- | --- | --- |
| `CASRequestAttempt` | `Number` | Number of physical requests the CAS request contract started. Each one was admitted by the mount fence and reserved against the call's deadline before it was sent. |
| `CASRequestConflictPause` | `Number` | Number of clean lost races the CAS request contract repaid after a flat jitter instead of a growing backoff: the resolve read had settled the conflict and no transport fault preceded it. |
| `CASRequestConnectFailureHint` | `Number` | Number of CAS write attempts whose transport error named a failed connection (no free local port, refused or unreachable peer, connect timeout). Under a reissuing policy the engine reissues them after a flat pause without a settle read, when the deadline and the fence admit it. Growth means the server cannot open connections to the object store. |
| `CASRequestFenceLostPostWrite` | `Number` | Number of CAS writes that were proven durable but lost the mount fence before the call could claim them. A non-zero value indicates late responses after the mount lifecycle changed. |
| `CASRequestFirstAttemptFuse` | `Number` | Number of CAS control requests whose first HTTP attempt matched the adaptive first-attempt timeout; the engine reissues them at once as attempt 2 when the policy and the gates permit. Growth means the object store does not answer a fresh connection within the first-attempt timeout. |
| `CASRequestGaveUp` | `Number` | Number of CAS writes that ended without a proven outcome, at a deadline, on a lost mount fence, or unresolved. A non-zero value means callers are being asked to retry later. |
| `CASRequestRefused` | `Number` | Number of CAS writes the store itself refused, proving they never applied: a malformed request, an entity too large, or an access or credential denial that no credential refresh was performed for, either because the disk has no refresh mechanism or because this write had already spent its one refresh. |
| `CASRequestReissue` | `Number` | Number of CAS requests re-sent: after a jittered backoff for an ordinary failure, after a flat pause for a connect-failure hint, or at once with no pause at all for a first-attempt fuse. Growth means the object store is throttling, failing, or contended. |
| `CASRequestResolveRead` | `Number` | Number of requests the CAS request contract made to settle a refused precondition or an ambiguous write: a body read, or a HEAD where the caller needs only presence. A connect-hinted attempt reissues without one. |

#### Conditional writes {#cas-conditional-writes}

These counters cover conditional writes made without transparent SDK retries.

| Event | Type | Description |
| --- | --- | --- |
| `CASConditionalWriteAttempts` | `Number` | Number of CAS conditional-write HTTP attempts made without transparent SDK retries. Growth indicates CAS metadata or reference updates. |
| `CASConditionalWriteCommitted` | `Number` | Number of CAS conditional writes completed successfully. Growth indicates successful conditional-update activity. |
| `CASConditionalWriteDefiniteFailure` | `Number` | Number of CAS conditional writes rejected with certainty before applying. A non-zero value indicates invalid requests, oversized entities, or access denial. |
| `CASConditionalWriteFenceLostPostWrite` | `Number` | Number of CAS writes that succeeded but lost the final mount-fence check. A non-zero value indicates late responses after the mount lifecycle changed. |
| `CASConditionalWriteUnresolved` | `Number` | Number of CAS conditional writes with an unknown outcome after conflict, timeout, connection loss, or server error. A non-zero value indicates backend instability or state requiring resolution. |

#### Hot-key lane {#cas-hot-key}

These counters cover serialization of writers that share one key.

| Event | Type | Description |
| --- | --- | --- |
| `CASHotKeyCacheStarts` | `Number` | Number of hot-key lane holds that started from the pool's last known object instead of a read. |
| `CASHotKeyCacheVerdictsReread` | `Number` | Number of verdicts (a refusal or a decline) a hot-key lane decide rendered on a cached object and that were re-rendered on a fresh read instead of delivered. |
| `CASHotKeyQueueWaitMicroseconds` | `Microseconds` | Total time CAS writers of a shared key spent queued in the hot-key lane before holding it or leaving, in microseconds. A rising value with a flat write rate means the holder is slow, not the store. |
| `CASHotKeyReadStarts` | `Number` | Number of hot-key lane holds that started from a read of the key. |

#### Part-folder views {#cas-part-folder}

These counters cover the cache of part-folder views built from part manifests.

| Event | Type | Description |
| --- | --- | --- |
| `CASPartFolderManifestGets` | `Number` | Number of part-manifest body GET requests used to build or validate folder views. High values indicate cache misses or validation work. |
| `CASPartFolderViewHits` | `Number` | Number of validated hits in the CAS part-folder view cache. Higher values indicate effective cache reuse. |
| `CASPartFolderViewInvalidations` | `Number` | Number of part-folder view cache erases after writes, ref drops, or namespace drops. Growth indicates metadata changes. |
| `CASPartFolderViewMisses` | `Number` | Number of CAS part-folder view builds with no retained entry. Growing values indicate low cache reuse or folder churn. |
| `CASPartFolderViewOversizedBypasses` | `Number` | Number of part-folder views not retained because they exceeded the entry size limit. A non-zero value indicates large folders reduce cache effectiveness. |
| `CASPartFolderViewValidationMismatches` | `Number` | Number of part-folder view validation mismatches that required rebuilding. A non-zero value indicates concurrent manifest changes or stale views. |

#### Relink confirmation {#cas-relink}

These counters cover fetch-by-relink confirms that this server answered `Unknown`.

| Event | Type | Description |
| --- | --- | --- |
| `CASRelinkConfirmRefusedLaneBroken` | `Number` | Number of CAS fetch-by-relink confirms answered Unknown because the namespace's ref lane is in NeedsRecovery, Closed or Faulted state, or is Writing with nothing carved. A growing value outside induced faults is a lane defect, not load. |
| `CASRelinkConfirmRefusedLaneWedged` | `Number` | Number of CAS fetch-by-relink confirms answered Unknown because the namespace's ref lane holds an unresolved append (a wedge). Lasts until the next flush or a remount resolves it. |
| `CASRelinkConfirmRefusedMountCannotSpeak` | `Number` | Number of CAS fetch-by-relink confirms answered Unknown because this mount cannot speak for the namespace: its ref table is unrecovered or mid-recovery, its catalog life was invalidated, its runtime was superseded by a remount, or its mount fence is no longer held. Neither a lane defect nor write load. A growing value means this writer is losing, or has already lost, its claim to the namespace. |
| `CASRelinkConfirmRefusedRefMutationInFlight` | `Number` | Number of CAS fetch-by-relink confirms this server answered Unknown because a queued or in-flight ref-lane mutation names the asked-about ref or the whole namespace. Expected under write load; the receiver retries the fetch. |
| `CASRelinkConfirmRefusedStateLockBusy` | `Number` | Number of CAS fetch-by-relink confirms answered Unknown because the ref table's state lock was held. Under write load the usual holder is the table's own append leader, arming or installing a chunk; otherwise a recovery, a listing or a snapshot publish. The confirm never waits for it. |

### Data lakes {#data-lakes}

These counters cover catalog calls and Iceberg metadata parsing.

#### Glue catalog {#glue-catalog}

These counters cover requests to an Iceberg Glue catalog.

| Event | Type | Description |
| --- | --- | --- |
| `DataLakeGlueCatalogCreateDatabase` | `Number` | Number of 'create database' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogCreateDatabaseMicroseconds` | `Microseconds` | Total time of 'create database' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogCreateTable` | `Number` | Number of 'create table' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogCreateTableMicroseconds` | `Microseconds` | Total time of 'create table' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogDropTable` | `Number` | Number of 'drop table' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogDropTableMicroseconds` | `Microseconds` | Total time of 'drop table' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogGetDatabases` | `Number` | Number of 'get databases' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogGetDatabasesMicroseconds` | `Microseconds` | Total time of 'get databases' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogGetTable` | `Number` | Number of 'get table' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogGetTableMicroseconds` | `Microseconds` | Total time of 'get table' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogGetTables` | `Number` | Number of 'get tables' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogGetTablesMicroseconds` | `Microseconds` | Total time of 'get tables' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogUpdateTable` | `Number` | Number of 'update table' requests to Iceberg Glue catalog. |
| `DataLakeGlueCatalogUpdateTableMicroseconds` | `Microseconds` | Total time of 'update table' requests to Iceberg Glue catalog. |

#### REST catalog {#rest-catalog}

These counters cover requests to an Iceberg REST catalog, including access-token handling.

| Event | Type | Description |
| --- | --- | --- |
| `DataLakeRestCatalogAuthTokenCachedValid` | `Number` | Number of requests to Iceberg REST catalog that reused a cached access token and did not fetch a new one. |
| `DataLakeRestCatalogAuthTokenRefreshedMicroseconds` | `Microseconds` | Total time spent fetching access tokens for Iceberg REST catalog. |
| `DataLakeRestCatalogAuthTokenRetrieve` | `Number` | Number of new access tokens fetched for Iceberg REST catalog (OAuth client-credentials or GCP metadata/ADC). |
| `DataLakeRestCatalogCreateNamespace` | `Number` | Number of 'create namespace' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogCreateNamespaceMicroseconds` | `Microseconds` | Total time of 'create namespace' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogCreateTable` | `Number` | Number of 'create table' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogCreateTableMicroseconds` | `Microseconds` | Total time of 'create table' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogCredentialsCacheHits` | `Number` | Number of table metadata requests to REST catalog reusing cached storage credentials. |
| `DataLakeRestCatalogCredentialsVended` | `Number` | Number of table metadata requests to REST catalog asking to vend storage credentials. |
| `DataLakeRestCatalogDropTable` | `Number` | Number of 'drop table' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogDropTableMicroseconds` | `Microseconds` | Total time of 'drop table' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetCredentials` | `Number` | Number of 'get credentials' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetCredentialsMicroseconds` | `Microseconds` | Total time of 'get credentials' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetNamespaces` | `Number` | Number of 'get namespaces' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetNamespacesMicroseconds` | `Microseconds` | Total time of 'get namespaces' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetTableMetadata` | `Number` | Number of 'get table metadata' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetTableMetadataMicroseconds` | `Microseconds` | Total time of 'get table metadata' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetTables` | `Number` | Number of 'get tables' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogGetTablesMicroseconds` | `Microseconds` | Total time of 'get tables' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogLoadConfig` | `Number` | Number of 'load config' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogLoadConfigMicroseconds` | `Microseconds` | Total time of 'load config' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogUnauthorized` | `Number` | Number of Iceberg REST catalog HTTP requests retried with a new access token after HTTP 401 or 403. |
| `DataLakeRestCatalogUpdateTable` | `Number` | Number of 'update table' requests to Iceberg REST catalog. |
| `DataLakeRestCatalogUpdateTableMicroseconds` | `Microseconds` | Total time of 'update table' requests to Iceberg REST catalog. |

#### Unity catalog {#unity-catalog}

These counters cover requests to an Iceberg Unity catalog.

| Event | Type | Description |
| --- | --- | --- |
| `DataLakeUnityCatalogGetCredentials` | `Number` | Number of 'get credentials' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetCredentialsMicroseconds` | `Microseconds` | Total time of 'get credentials' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetSchemas` | `Number` | Number of 'get schemas' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetSchemasMicroseconds` | `Microseconds` | Total time of 'get schemas' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetTable` | `Number` | Number of 'get table' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetTableMetadata` | `Number` | Number of 'get table metadata' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetTableMetadataMicroseconds` | `Microseconds` | Total time of 'get table metadata' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetTableMicroseconds` | `Microseconds` | Total time of 'get table' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetTables` | `Number` | Number of 'get tables' requests to Iceberg Unity catalog. |
| `DataLakeUnityCatalogGetTablesMicroseconds` | `Microseconds` | Total time of 'get tables' requests to Iceberg Unity catalog. |

#### Metadata file parsing {#iceberg-parsing}

These counters record how many Iceberg metadata files were parsed and how long parsing took.

| Event | Type | Description |
| --- | --- | --- |
| `IcebergAvroFileParsing` | `Number` | Number of times avro metadata files have been parsed. |
| `IcebergAvroFileParsingMicroseconds` | `Microseconds` | Time spent for parsing avro metadata files for Iceberg tables. |
| `IcebergJsonFileParsing` | `Number` | Number of times json metadata files have been parsed. |
| `IcebergJsonFileParsingMicroseconds` | `Microseconds` | Time spent for parsing json metadata files for Iceberg tables. |

### Export {#export}

These counters cover part and partition export.

#### Part export {#part-export}

These counters cover background [`EXPORT PART`](/antalya/part_export) tasks. The number of exports
running right now is the [`Export`](#export-metrics) metric.

| Event | Type | Description |
| --- | --- | --- |
| `ExportPartsRejectedByMemoryLimit` | `Number` | Number of background export part tasks rejected due to background memory limit. |
| `ExportsThrottlerBytes` | `Bytes` | Bytes passed through 'max_exports_bandwidth_for_server' throttler. |
| `ExportsThrottlerSleepMicroseconds` | `Microseconds` | Total time a query was sleeping to conform 'max_exports_bandwidth_for_server' throttling. |
| `PartsExportDuplicated` | `Number` | Number of part exports that failed because target already exists. |
| `PartsExportFailures` | `Number` | Number of failed part exports. |
| `PartsExportTotalMilliseconds` | `Milliseconds` | Total time spent on part export operations. |
| `PartsExports` | `Number` | Number of successful part exports. |

#### Partition export {#partition-export}

These counters cover ZooKeeper requests made by [`EXPORT PARTITION`](/antalya/partition_export).

| Event | Type | Description |
| --- | --- | --- |
| `ExportPartitionZooKeeperCreate` | `Number` | Number of 'create' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperExists` | `Number` | Number of 'exists' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperGet` | `Number` | Number of 'get' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperGetChildren` | `Number` | Number of 'getChildren' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperGetChildrenWatch` | `Number` | Number of 'getChildrenWatch' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperGetWatch` | `Number` | Number of 'getWatch' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperMulti` | `Number` | Number of 'multi' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperRemove` | `Number` | Number of 'remove' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperRemoveRecursive` | `Number` | Number of 'removeRecursive' requests to ZooKeeper made by the export partition feature. |
| `ExportPartitionZooKeeperRequests` | `Number` | Total number of ZooKeeper requests made by the export partition feature. |
| `ExportPartitionZooKeeperSet` | `Number` | Number of 'set' requests to ZooKeeper made by the export partition feature. |

### Object storage {#object-storage}

These counters cover cluster dispatch, the list cache, and per-call object-store timings.

#### Object storage cluster {#object-storage-cluster}

These counters cover task dispatch for `object_storage_cluster` and the cluster table functions.
See [Swarm](/antalya/swarm).

| Event | Type | Description |
| --- | --- | --- |
| `ObjectStorageClusterProcessedTasks` | `Number` | Number of processed tasks in ObjectStorageCluster request. |
| `ObjectStorageClusterSentToMatchedReplica` | `Number` | Number of tasks in ObjectStorageCluster request sent to matched replica. |
| `ObjectStorageClusterSentToNonMatchedReplica` | `Number` | Number of tasks in ObjectStorageCluster request sent to non-matched replica. |
| `ObjectStorageClusterWaitingMicroseconds` | `Microseconds` | Time of waiting for tasks in ObjectStorageCluster request. |

#### List-objects cache {#list-objects-cache}

These counters cover hits and misses of the object-storage list cache.

| Event | Type | Description |
| --- | --- | --- |
| `ObjectStorageListObjectsCacheExactMatchHits` | `Number` | Number of times object storage list objects operation hit the cache with an exact match. |
| `ObjectStorageListObjectsCacheHits` | `Number` | Number of times object storage list objects operation hit the cache. |
| `ObjectStorageListObjectsCacheMisses` | `Number` | Number of times object storage list objects operation miss the cache. |
| `ObjectStorageListObjectsCachePrefixMatchHits` | `Number` | Number of times object storage list objects operation hit the cache using prefix matching. |

#### S3 and Azure request timing {#s3-azure-timing}

These counters cover client-side time of individual S3 and Azure list and head calls, plus refused transparent retries.

| Event | Type | Description |
| --- | --- | --- |
| `AzureListObjectsMicroseconds` | `Microseconds` | Time of Azure blob storage API ListObjects execution. |
| `S3HeadObjectMicroseconds` | `Microseconds` | Time of S3 API HeadObject execution. |
| `S3ListObjectsMicroseconds` | `Microseconds` | Time of S3 API ListObjects execution. |
| `S3SingleAttemptRetryConsultations` | `Number` | Number of AWS SDK retry consultations refused by the single-attempt S3 retry profile. Non-zero means the SDK attempted a transparent retry on a write that must make exactly one HTTP attempt. |

### Other profile events {#other-profile-events}

The remaining counters cover Puffin files, the Keeper changelog, the Naive Bayes classifier, the
polygon pool, and the reader executor.

#### Puffin files {#puffin-events}

These counters cover reads and cache traffic for Puffin files (deletion vectors and memoized
footers). Cache size is under [Puffin files cache](#puffin-metrics).

| Event | Type | Description |
| --- | --- | --- |
| `PuffinFileReadMicroseconds` | `Microseconds` | Total time spent reading Puffin files. |
| `PuffinFilesCacheHits` | `Number` | Number of times parsed Puffin file content has been found in the cache. |
| `PuffinFilesCacheMisses` | `Number` | Number of times parsed Puffin file content has not been found in the cache and had to be read from disk. |
| `PuffinFilesCacheWeightLost` | `Number` | Approximate number of bytes evicted from the Puffin files cache. |
| `PuffinFilesRead` | `Number` | Number of Puffin files read (footer or deletion vector blob). |

#### Keeper changelog {#keeper-changelog}

These counters cover Keeper log entries served from the commit cache or prefetched from the changelog.

| Event | Type | Description |
| --- | --- | --- |
| `KeeperLogsEntryReadFromCommitCache` | `Number` | Number of log entries in Keeper being read from commit logs cache |
| `KeeperLogsPrefetchedEntries` | `Number` | Number of log entries in Keeper being prefetched from the changelog file |

#### Naive Bayes classifier {#naive-bayes}

These counters cover models loaded for the Naive Bayes classifier.

| Event | Type | Description |
| --- | --- | --- |
| `NaiveBayesClassifierModelsAllocatedBytes` | `Bytes` | Number of bytes allocated for Naive Bayes Classifier models. |
| `NaiveBayesClassifierModelsLoaded` | `Number` | Number of Naive Bayes Classifier models loaded. |

#### Polygon pool {#polygon-pool}

These counters cover polygons cached for `pointInPolygon`.

| Event | Type | Description |
| --- | --- | --- |
| `PolygonsAddedToPool` | `Number` | A polygon has been added to the cache (pool) for the 'pointInPolygon' function. |
| `PolygonsInPoolAllocatedBytes` | `Bytes` | The number of bytes for polygons added to the cache (pool) for the 'pointInPolygon' function. |

#### Reader executor {#reader-executor}

This counter is the number of bytes the reader executor delivered to read requests.

| Event | Type | Description |
| --- | --- | --- |
| `ReaderExecutorRequestedBytes` | `Bytes` | Useful bytes ReaderExecutor delivered to read requests (the requested window payload, excluding over-read and cache write-back). Denominator for the modeled cost-per-byte KPI (ReaderExecutorModeledCostMicroseconds / ReaderExecutorRequestedBytes). |

## Metrics {#metrics}

### Content-addressed storage {#cas-metrics}

These gauges are the thread-pool occupancy and cache sizes for
[content-addressed storage](/antalya/cas).

| Metric | Description |
| --- | --- |
| `CASBlobUploadPoolThreads` | Number of threads in the CA blob upload thread pool. |
| `CASBlobUploadPoolThreadsActive` | Number of threads in the CA blob upload thread pool running a task. |
| `CASBlobUploadPoolThreadsScheduled` | Number of queued or active jobs in the CA blob upload thread pool. |
| `CASHotKeyCacheBytes` | Bytes retained by the CA hot-key lane's cache of last known objects |
| `CASHotKeyCacheEntries` | Entries retained by the CA hot-key lane's cache of last known objects |
| `CASManifestDecodeCacheBytes` | Bytes retained by the CA manifest decode cache |
| `CASManifestDecodeCacheEntries` | Entries retained by the CA manifest decode cache |
| `CASPartFolderCacheBytes` | Estimated bytes retained by the CA part-folder view cache |
| `CASPartFolderCacheEntries` | Entries retained by the CA part-folder view cache |

### Export {#export-metrics}

This gauge is the number of exports currently running. Completed, failed, and duplicated exports
are [profile events](#part-export).

| Metric | Description |
| --- | --- |
| `Export` | Number of currently executing exports |

### Swarm {#swarm-metrics}

This gauge reports whether [swarm mode](/antalya/swarm) is enabled on this server. `0` means
disabled and `1` means enabled.

| Metric | Description |
| --- | --- |
| `IsSwarmModeEnabled` | Indicates if the swarm mode enabled or not: 0 = disabled, 1 = enabled |

### Puffin files cache {#puffin-metrics}

Size of the Puffin files cache. Read and hit counters are [profile events](#puffin-events).

| Metric | Description |
| --- | --- |
| `PuffinFilesCacheBytes` | Size of the Puffin files cache in bytes (deletion vectors plus memoized footers) |
| `PuffinFilesCacheFiles` | Number of cached entries in the Puffin files cache (deletion vectors plus memoized footers) |
