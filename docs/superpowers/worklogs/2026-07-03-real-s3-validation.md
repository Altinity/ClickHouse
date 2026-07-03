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

## GCS leg — measurement complete (2026-07-03, same day) {#gcs-measurement}

Bucket `content-adressable-test-mfilimonov` (service-account HMAC pair in git-ignored
`configs/gcs.env`; stand: `docker-compose-gcs.yml` + `storage_conf_gcs_*.xml`).

**Verdict: GCS's S3-compatible surface enforces NONE of the conditional ops CAS needs, but the
same HMAC pair over the same XML endpoint with GOOG4-HMAC-SHA256 signing enforces ALL of them.**

| Capability | AWS S3 (sigv4) | GCS S3-compat (sigv4) | GCS XML + GOOG4 signing |
|---|---|---|---|
| create-if-absent (`If-None-Match: *` / `x-goog-if-generation-match: 0`) | 412 enforced | **silently ignored** (probe step 2 caught it; mount refused, exit 48) | 412 enforced |
| conditional overwrite (`If-Match` / generation) | 412 enforced | **silently ignored** | 412 enforced |
| conditional delete (`If-Match` / generation) | 412 enforced | **silently ignored — wrong-ETag DELETE deleted the object** | 412 enforced |
| mixing `x-goog-*` preconditions into sigv4 requests | n/a | rejected: `ExcessHeaderValues` ("cannot specify both x-amz and x-goog") | n/a (all headers are `x-goog-*`) |
| token in PUT response | ETag | — | `x-goog-generation` (no extra HEAD needed) |

Full 12-step battery: `utils/ca-soak/scripts/gcs_goog4_probe.py` (12/12 OK — create-if-absent,
wrong/correct-generation overwrite and delete, body-intact checks, HEAD generation, 404 after
delete, generation changes on every write).

Follow-up battery (`gcs_goog4_mp_probe.py`, same day): `CompleteMultipartUpload` **silently
ignores** `x-goog-if-generation-match` (both `0`-against-existing and a wrong generation returned
200 and overwrote) — conditional writes must not use multipart on GCS. `Compose` **enforces** the
precondition (5/5) — the production-grade big-blob path is multipart-to-temp-key (unconditional)
-> `Compose(temp -> final)` with the precondition -> delete temp. The 412 XML body code is
literally `PreconditionFailed` — the same string the ClickHouse-side detectors already match.

Design implication for the `TokenType::Generation` binding: GOOG4-HMAC-SHA256 is structurally
sigv4 with renamed constants (`GOOG4` key prefix, `goog4_request` scope, `x-goog-date`/
`x-goog-content-sha256` headers) — a signer variant plus a precondition-header mapping
(`If-None-Match: *` -> `x-goog-if-generation-match: 0`, `If-Match: <etag>` ->
`x-goog-if-generation-match: <generation>`) and token extraction from `x-goog-generation` could
carry the ENTIRE existing CAS backend to GCS natively. Not started — needs a brainstorm/spec pass.

## GCS leg — binding implemented + full cycle GREEN (2026-07-03, same day) {#gcs-cycle}

The Generation binding landed (spec `2026-07-03-cas-gcs-generation-binding-design`, 9-task SDD
run, final whole-branch review MERGE-READY): GOOG4-HMAC signer, wire-boundary conditional dialect,
`http_client = gcs_hmac` opt-in, `TokenType::Generation` stamping, single-PUT guard, probe
versioning fail-close. Live cycle on prefix `ca_live_20260703_g2`: probe PASS (first CAS mount on
GCS), replication + equal checksums (100k + 250k churn), two-phase reclaim observed against the
bucket (11 condemned -> graduated -> deleted), `DROP TABLE` to 51 objects / 20.8 KB pool metadata,
final fsck all-zero, `dedup_ratio=2`, `blob_storage_log` 100 deletes / 0 errors.

Three bugs found live and fixed in-branch (each one caught fail-closed by the dialect/probe, no
data damage): (1) probe wrong-tokens were non-numeric -> dialect format guard killed the mount
(`1f58e7f2fef`); (2) LIST-derived tokens are MD5 ETags on GCS (XML LIST bodies are not rewritten)
-> poisoned `If-Match` in GC; list tokens now disabled on generation stores (`86f44c8061c`);
(3) `Expect: 100-continue` did not recognize the dialect header -> B118-class stall risk
(`01b4b92a945`, from the final review). Plus one stand/product finding: a `ca_ro` shadow disk in
the server config breaks table load on restart (`UNKNOWN_DISK`) — workaround shipped
(`fsck_only_gcs.xml`), product fix is a ROADMAP prod-gate row.

## Remaining for gate #1 {#remaining}

Azure: not started. GCS production-grade follow-ups (compose finalize for >1 GiB conditional
writes, `gcp_oauth` validation, generation-aware LIST discovery) are DESIRABLE rows.
