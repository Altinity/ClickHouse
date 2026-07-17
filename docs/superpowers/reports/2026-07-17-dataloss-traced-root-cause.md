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
