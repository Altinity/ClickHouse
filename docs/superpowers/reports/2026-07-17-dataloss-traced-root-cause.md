# CRITICAL data-loss — REPRODUCED + TRACED root cause (2026-07-17)

## Status: CONFIRMED real, REPRODUCED deterministically, mechanism now GROUNDED in logs (earlier orphan-CAS-token theory REFUTED).

## Reproduction (deterministic)
`build/dl_probe.py` on a preserved CA cluster (docker-compose.yml, R3 binary): 8 workers insert distinct sync+dedup rows CONTINUOUSLY for 150s; mid-stream fault = `docker pause rustfs1` for 105s (> the 90s CAS write budget) + `docker kill ca-soak-ch2-1` then `docker start` (replica kill during the S3 outage). Result:
- **submitted=1314, acked=1314 (every insert got HTTP 200), PRESENT=1116, LOST(acked-but-absent)=198** (contiguous ids 1117..~1314 — the block inserted during the fault window). No TRUNCATE/TTL confounder (single plain table).

## Smoking gun (system.text_log, ch1, fault window ~05:43 UTC; 9774 such lines)
```
Block with ID all_10341432382904313846_13999145979494917125 already exists on other replicas as part all_1117_1117_0; ignoring it.
```
(same for all_1116/1118/1120/1121/... — one per retried lost id). Part `all_1117_1117_0` is verified ABSENT from BOTH replicas.

## Mechanism (grounded — replaces the refuted orphan-token theory)
1. During the fault, an insert attempt for id 1117 reaches the ReplicatedMergeTree commit and registers the **block_id znode + part-znode `all_1117_1117_0`** in Keeper (Keeper stayed up; the sink's commit multi ran). NOTE this refutes both (a) the original "orphan CAS dedup token" theory and (b) dedupTrace's "a failed CAS write leaves no token" (the token DID get created — the RMT block-dedup token is a Keeper znode, independent of the CAS blob durability).
2. But the part's **CAS data (blobs/manifest) did not durably land** — rustfs (the S3 backend) was PAUSED, so the blob PUTs never durably reached it — AND the committing replica (ch2) was then KILLED. So no replica holds the part data: it is a **PHANTOM part** (Keeper: exists; disk/S3: nowhere).
3. The client (soak driver / any client) received R3's `NETWORK_ERROR` ("stageManifest UNCERTAIN, retry-later") and **retries the byte-identical INSERT**. Same content ⇒ same `block_id`. RMT cross-replica dedup finds the phantom block_id znode ⇒ **"already exists on other replicas as part all_1117_1117_0; ignoring it"** ⇒ returns SUCCESS without re-inserting.
4. Net: 198 rows the server acked as written have no data anywhere. `fsck` stays clean (the CAS layer has no dangling ref — nothing was ever referenced), so the CA integrity oracle cannot see it; only a row-count/checksum oracle catches it.

## R3 (#37) connection
R3 changed the ambiguous-CAS-write abort from an internal same-request ABORTED retry to a client-visible `NETWORK_ERROR` "retry-later", which is what makes the CLIENT re-issue a byte-identical INSERT — the retry that then dedups against the phantom. Whether this loss also occurs pre-R3 (generic RMT+CAS + S3-outage + replica-kill hazard, just less frequently) is NOT yet established — the block-dedup + phantom-part interaction is generic RMT behavior on CA storage; R3 plausibly increases its frequency by turning the ambiguous case into a client retry. TODO: repro on a pre-R3 binary to separate "R3 introduced" from "R3 amplified".

## STILL TO VERIFY (deeper trace on the preserved cluster — do before teardown)
- `system.part_log`: was `all_1117_1117_0` NewPart'd, on which replica, and RemovePart'd? (confirms who committed it + that the data was dropped/never-materialized)
- `system.blob_storage_log`: the S3 PUT ops for `all_1117_1117_0`'s blobs — did any durably succeed, or all fail/hang during the pause?
- `content_addressed_log`: blob_put / ref-publish / reuse events + outcomes for that part's manifest.
- Whether the phantom part-znode is ever reconciled (does ClickHouse eventually detect "part in Keeper but no replica has it"? a LOST_PART / detach? or does it stay phantom forever?).

## Fix direction (do NOT land blind — hard, likely touches generic RMT+CAS commit atomicity)
The invariant violated: **a block_id/part-znode must not be durably registered in Keeper until the part's data is durably readable by at least one live replica.** On CA storage the "data durable" means the CAS blobs+manifest durably in S3. Candidate directions: (a) order the Keeper block-commit AFTER the CAS blob/manifest durability is confirmed (not just staged); (b) on the ambiguous-CAS-write case, do NOT let the block_id persist (roll it back) so a retry re-inserts; (c) reconsider R3 for the ambiguous case (keep it an internal retry that does not cross the client/dedup boundary). Needs a source trace of ReplicatedMergeTreeSink commit-multi timing vs CAS blob durability + a decision. Repro: `build/dl_probe.py` (preserved cluster) — reliably yields ~15% loss under pause+kill.

## Deeper trace (part_log + blob_storage_log) — DONE 2026-07-17, refines the root
- `system.part_log` for `all_1117_1117_0`: ONLY `RemovePart` on ch1 (05:43:09 rows=1; 05:44:11 rows=0) — NO `NewPart` on either replica (ch2's NewPart was in its async part_log buffer, lost when ch2 was killed). So ch1 knew the part via Keeper/replication, never materialized its data, then removed it.
- `system.blob_storage_log` (fault window 05:41-05:45): Upload ok=5694, **Upload FAILED=112**, Delete=8822. The 112 failed blob uploads are the phantom parts' CAS data that never durably landed (rustfs paused).

REFINED ROOT: a part's **block_id dedup znode + part-znode are committed to Keeper**, but its **CAS blob PUTs fail** (S3 outage) and the committing replica is killed → the part is `RemovePart`'d (data broken/absent) **while the block_id dedup znode survives** → a byte-identical retry dedups against the stale block_id ("already exists on other replicas ... ignoring it") → acked, no data. The generic RMT invariant assumes a committed block's data is durable on the committing replica's LOCAL disk (survives a kill); on CA the data is on shared S3, which can fail to persist while Keeper commits + while the block_id dedup znode is created. The block_id dedup znode lifecycle is decoupled from CAS-blob durability — that is the core defect.

REMAINING TODO (unchanged priority): (1) pre-R3 repro to separate "R3 introduced" vs "R3 amplified" (the block_id/phantom interaction is generic RMT-on-CA; R3's client-visible NETWORK_ERROR retry is what drives the byte-identical re-insert that dedups); (2) exact source path where the block_id znode should be gated on CAS durability / cleaned with the RemovePart; (3) does ClickHouse ever reconcile a Keeper-known part that no replica holds (LOST_PART detach)? Repro harness: build/dl_probe.py on the preserved cluster.

## DECISIVE (2026-07-17, direct code+Keeper answer to "is the block written to the dedup znode before durable confirmation?")
NO — the write-path ordering is CORRECT. In ReplicatedMergeTreeSink::commitPart: `getCommitPartOps` builds the block_id znode into `ops` (:952); `transaction.renameParts()` (:976) runs the CA commit → ContentAddressedTransaction::commit → publishStaging, where `uploadPendingBlobs` (blobs) + `promoteBuild` (publish ref) run and THROW on failure (fail-closed, ContentAddressedTransaction.cpp:356-363); ONLY THEN the Keeper multi writing the block_id znode runs (:985). A failed/uncertain CAS write throws at :976 so :985 is never reached → no znode for that attempt. So the block_id znode is written AFTER durable CAS confirmation, not before.

THE ACTUAL DEFECT is the OPPOSITE end — the block_id dedup znode OUTLIVES the part's data on the REMOVAL path. PROOF (system.zookeeper, preserved cluster): /clickhouse/tables/dl_probe/blocks has **1258 block_id znodes** but only **1116 rows / 2 active parts** present — the block znodes for the lost ids (ctime 05:41-05:43 = fault window) still exist in Keeper while their data is gone. The block_id dedup znode lifetime is the rolling `replicated_deduplication_window` (count/time based), DECOUPLED from the part-data lifetime: when a part is removed (RemovePart, here after the fault killed its committing replica) its CAS manifest+blobs are GC-deleted (manifest_delete=3265, ref_drop=112, blob_delete=60 in the window) but its block_id znode SURVIVES. A byte-identical retry (R3-NETWORK_ERROR-driven) dedups against the stale znode -> "already exists on other replicas ... ignoring it" -> acked, no data.

Why generic ClickHouse is safe but CA is not: generic RMT assumes a committed block's data is durable on the COMMITTING REPLICA'S LOCAL DISK (survives a kill; part re-fetchable), so a surviving block_id znode always points at recoverable data. On CA the data is shared S3; once the part is removed and its blobs GC-reclaimed, no replica can recover it, yet the dedup znode still says "exists".

FIX DIRECTION (revised, precise): NOT a write-path ordering change. Options: (a) invalidate/remove the block_id dedup znode when the part is removed or detected LOST (tie znode lifetime to part-data lifetime on CA); (b) on CA, the RMT block-dedup "already exists, ignoring" must VERIFY the referenced part's data is actually present/recoverable before skipping the insert (do not trust the znode alone on shared storage); (c) the CA-specific "part in Keeper but no replica holds recoverable data" must trigger a LOST_PART/re-insert rather than a silent dedup-skip. STILL TODO: the exact removal trigger (why the durably-committed part is RemovePart'd during the replica-kill fault — part_log/replication_queue), and pre-R3 repro (R3 amplifies via client retry; the znode-outlives-data leak is generic-RMT-on-CA).
