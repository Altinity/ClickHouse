# 2026-07-03 — Real-S3 (AWS) validation run {#real-s3-validation-20260703}

Release-gate #1 (AWS part). Bucket `test-altinity-support-team` (us-east-1), prefixes
`ca_live_20260703_r1` … `_r3`. Stand: `utils/ca-soak/docker-compose-awss3.yml` — two replicas +
keeper, shared CA pool on live AWS, credentials via git-ignored `configs/aws.env`
(`use_environment_credentials`), named volumes for `/var/lib/clickhouse`.

## Verdict {#verdict}

AWS S3 is a fully conforming CAS store. Everything the protocol demands worked on the first try:
the capability probe passes with honest 412s on wrong-token conditional ops, two-phase reclaim
(condemn → ack-floor graduation → delete) deletes for real, fsck is clean at every stage, and no
clamp/false-404/anomaly appeared in 28+ GC rounds (the RustFS #3231 pathology does not exist here).

## Timeline / measurements (r1) {#timeline-r1}

| Step | Bucket state | Checks |
|---|---|---|
| Fresh mount + smoke (100k rows, ch1→ch2) | 31 objects / 10.4 MB | probe OK; checksums equal |
| 6 inserts + `OPTIMIZE FINAL` churn | 64 obj / 73.8 MB | GC rounds Success, 0 anomalies |
| Old parts dropped (both replicas, ~340 s) | — | condemn 11 (round 24) → graduate (26) → **delete 11 (27)** |
| After reclaim | 44 obj / 37.3 MB | fsck: `dangling=0 unreachable=0 unaccounted=0`, **`dedup_ratio=2`** (73.1 MB logical / 36.5 MB physical — two replicas, one copy) |
| `DROP TABLE` on both | **35 obj / 13.9 KB** | full reclaim to pool metadata; fsck all-zero |

GC round duration on live S3: 30–40 s (vs ~5–10 s on RustFS) — pure request latency; the shipped
default `gc_interval_sec=60` is sane, the soak's 10 s just runs back-to-back.

## Bugs found and fixed during the run {#fixes}

1. **Factory `root_shards` default was silently 8** — the weighed 32 landed only in `PoolConfig`;
   `MetadataStorageFactory` overrode it. Fixed to 32; verified live on r3 (shard indexes up to 27).
2. **`server_root_id` macro expansion** — one config template per stand now works:
   `<server_root_id>{replica}</server_root_id>` (same expansion as the s3 `endpoint`). Verified
   live (`ca_live_node1/node2` in the pool layout).
3. **Probe cleanup 400s** — the exit cleanup issued `deleteExact` with an EMPTY `If-Match` after the
   happy path had already deleted the probe keys; AWS answers 400 `InvalidArgument`, printing two
   `AWSClient <Error>` lines per mount. HEAD-gated now; red-checked
   (`CasProbe.CleanupNeverDeletesWithEmptyToken`).
4. **`blob_storage_log` blind to GC reclaim** — `removeObjectIfTokenMatches` had no
   `BlobStorageLogWriter`: the live run showed 34 `Upload` / 0 `Delete` while GC deleted 28 objects.
   Conditional deletes are now logged (all outcomes, S3 error attached).
5. **Foreign-owner refusal UX** — recreating a container without a persistent `/var/lib/clickhouse`
   mints a new `ServerUUID`, and the pool refuses the root claim (correct fail-close; hit live on
   r2 restart, exit code 246 = `CORRUPTED_DATA`). The exception now names both uuids and the three
   recovery paths, mirroring `mountDoubleStartMessage`. Compose moved to named volumes (= PVC).
6. **Dead config keys removed** from all soak configs: `content_addressed_gc_grace_sec`,
   `content_addressed_allow_shared_pool` (neither is parsed anymore).

## Notes for the production test stand {#test-stand-notes}

`tmp/test_stand_ca_storage.xml` — CA storage config for the operator-managed stand: endpoint WITHOUT
`{replica}` (shared pool per cluster/shard), `server_root_id={replica}`, cache layer kept (immutable
content-hash keys cache perfectly; control-plane refs bypass the cache by construction),
`prefer_not_to_merge` dropped, all CA settings listed with defaults commented out.

`/var/lib/clickhouse` MUST be persistent (PVC): the local uuid file is the CAS identity.

## Remaining for gate #1 {#remaining}

GCS (needs the `Generation` token binding — currently fail-closed-unprobed) and Azure.
