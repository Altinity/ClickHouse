---
description: 'Real-object-storage release-gate implementation and evidence for unconditional CAS blob publication.'
sidebar_label: 'CAS blob publication live results'
sidebar_position: 90
slug: /superpowers/cas/unconditional-blob-publication-live-results
title: 'CAS unconditional blob publication live results'
doc_type: 'reference'
---

# CAS unconditional blob publication live results {#cas-unconditional-blob-publication-live-results}

## Decision {#decision}

Implementation status is `DONE_WITH_CONCERNS`; release readiness is **blocked**.

The real Google Cloud Storage gate collected every required live case, but no Google credential
source was available and all 25 live cases skipped. Two credential-free evidence-safety regressions
passed. Therefore this run contains no evidence that Google accepted the new live request shapes.
The AWS-compatible CAS lane passed both cases. The ordinary S3 lane was blocked during fixture setup
because its historical ClickHouse image is unavailable; its partial result is not green.

Deterministic fake-GCS evidence remains useful for syntax and request classification, but it is not a
substitute for this missing provider acceptance.

## Environment classification {#environment-classification}

| Property | Observation |
|---|---|
| Evidence date | 2026-08-23 |
| Source base | `12079eedd47ac59917764ba0f17f6d67c12c2346` on `cas-gc-rebuild` |
| Live provider | Google Cloud Storage |
| Endpoint class | Public Google XML API, `https://storage.googleapis.com` |
| `gcs_hmac` credential gate | Absent |
| `gcp_oauth` metadata credential gate | Absent |
| `gcp_oauth` ADC credential gate | Absent |
| Live bucket gate | Absent |
| TLS fault proxy and control gate | Absent |
| TLS fault-proxy public CA file gate | Absent |
| AWS-compatible provider | Local RustFS fixture |

Only presence or absence was inspected. No credential, authorization token, access key, secret, or
configuration value is included in this document or in the test output inspected for this result.

## Credential-free evidence hardening {#credential-free-evidence-hardening}

Two local regressions now run without a bucket or credential source. The first exercises the actual
regex-mode `ClickHouseInstance.grep_in_log` path with a representative batch-DELETE line and requires
both a literal-prefix match and rejection of an absent prefix. The production matcher escapes the
opening bracket in `Objects with paths [` so `zgrep` cannot reject it as an invalid expression.

The second injects a synthetic numeric generation through the CAS-log helper boundary. Raw
generation and ordinary ETag values stay inside helpers marked with `__tracebackhide__`; live test
frames receive only domain booleans and SHA-256 digests. CAS event dictionaries returned to tests do
not contain `token`, and assertion messages contain no raw provider value. This matters because the
Praktika pytest runner enables `--showlocals`.

## Implemented live scenario inventory {#implemented-live-scenario-inventory}

The eight parameterized test functions below encode nine logical publication scenarios. Each has a
`gcs_hmac` and a `gcp_oauth` case, for 18 scenario/authentication assertions when both credential
sources and the required fault driver are available.

| № | Required scenario | Live assertion seam | Authentication cases | Collected result |
|---:|---|---|---|---|
| 1 | Fresh streaming | One mandatory blob `HEAD` per fan-out task; target `blob_put` has `publication_reason=absent` and `transport=streaming` | `gcs_hmac`, `gcp_oauth` | 2 skipped |
| 2 | Duplicate adoption | Same target emits `blob_reuse_adopt`, no second `blob_put`, and remains readable | `gcs_hmac`, `gcp_oauth` | 2 skipped, paired with № 1 |
| 3 | Concurrent equivalent publishers | Two query-attributed writers touch a common hash, publish or adopt safely, and both tables read correctly | `gcs_hmac`, `gcp_oauth` | 2 skipped |
| 4 | Blob above the former cap | Target logical size exceeds the configured 5 MiB genuine-conditional ceiling and succeeds as a one-shot `Default` streaming PUT | `gcs_hmac`, `gcp_oauth` | 2 skipped |
| 5 | Multipart publication | Query-attributed `S3CreateMultipartUpload`, `S3UploadPart`, and `S3CompleteMultipartUpload` all occur for a `Default` body | `gcs_hmac`, `gcp_oauth` | 2 skipped |
| 6 | Native staged copy | First-plus-absent targets select `transport=server_side_copy`, with exactly one `S3CopyObject` request per successful native publication | `gcs_hmac`, `gcp_oauth` | 2 skipped |
| 7 | `Condemned` staged retagging | Manual GC writes `Condemned`; the same staged payload selects `publication_reason=condemned` and `transport=streaming` | `gcs_hmac`, `gcp_oauth` | 2 skipped |
| 8 | Queued old-token delete | The driver holds GC's already-authenticated old exact DELETE after the fold cut, the writer retags, Google rejects the released stale generation, and `system.cas_log` reports `blob_delete outcome=replaced` | `gcs_hmac`, `gcp_oauth` | 2 required skips |
| 9 | Ambiguous staged copy, absent retry, retagged replacement | External driver proves landed copy, response loss, exact old-generation deletion, retry miss, retagged PUT, and old-delete miss; `system.cas_log` independently reports retry `transport=streaming` | `gcs_hmac`, `gcp_oauth` | 2 required skips |

Publication decisions come from per-query rows in `system.cas_log`. Mandatory `HEAD`, multipart, and
copy reachability come from the same query's `system.query_log.ProfileEvents`. The GC race uses
synchronous `SYSTEM CAS GC RUN` rounds after stopping background schedulers, then correlates
`blob_retire`, `gc_recheck_verdict`, and `blob_delete` by content hash. These seams ensure unrelated
traffic cannot make a publication scenario green.

## TLS fault-driver contract {#tls-fault-driver-contract}

Timing a normal HTTPS request cannot reliably create either a landed write whose response is lost or
an old exact delete held across a replacement. The live test therefore requires credential-free
endpoints in `GCS_LIVE_AMBIGUITY_PROXY_URI` and `GCS_LIVE_AMBIGUITY_CONTROL_URL`, plus the public trust
anchor in `GCS_LIVE_AMBIGUITY_CA_FILE`. The endpoints must be credential-free HTTP(S) URLs without a
query or fragment. The CA file is copied into the ClickHouse container; strict certificate
verification and the image's default CA roots remain enabled. The proxy must remain transparent until
armed for the dedicated per-run fault prefix. The test disables AWS SDK retries only for the
fault-controlled endpoint prefixes, so the first lost response reaches the production CAS retry state
machine while ordinary real-GCS operations retain their default retry profile.

For queued-delete evidence, `POST /v1/queued-old-delete/arm` accepts only a scenario identifier,
non-secret object prefix, and target content hash. It holds the next already-authenticated exact
DELETE for that target after the GC fold has excluded the later writer. The test waits through
`POST /v1/queued-old-delete/status`, lets the production writer replace the `Condemned` body, and
calls `POST /v1/queued-old-delete/release`. The driver forwards the original request unchanged.
`POST /v1/queued-old-delete/result` must then report that Google returned a precondition mismatch and
that the replacement remains present; it must expose neither the signed request nor its generation.

`POST /v1/staged-copy-ambiguity/arm` accepts only a scenario identifier, authentication-mode label,
and non-secret object prefix. For the first native copy under that prefix, the driver must:

1. let Google accept the copy;
2. suppress the response to ClickHouse;
3. exact-delete the landed generation before the retry `HEAD`;
4. let the absent retry and retagged streaming PUT complete;
5. retry the old exact delete after replacement and observe that it misses; and
6. prove the replacement is still present.

`POST /v1/staged-copy-ambiguity/result` returns those six phase booleans, the response-loss boolean,
and the target content hash. It must not return credentials or authorization material. Provider
credentials required by its deletion control plane remain inside the operator-owned driver. Missing
driver endpoints or public CA file produce required skips for both fault-controlled scenarios and
block release.

## Ordinary non-CAS characterization {#ordinary-non-cas-characterization}

The ordinary real-GCS matrix retains the pre-change `Default` contract for both authentication modes:

| Operation or observation | Live test mechanism | Cases in this run |
|---|---|---:|
| PUT and metadata-bearing verification `HEAD` | MergeTree insert with `s3_check_objects_after_upload` | 2 skipped |
| GET | Count and aggregate reads from the ordinary disk | 2 skipped |
| LIST | Two named-collection Parquet objects read through one glob | 2 skipped |
| Native copy | `ALTER TABLE ... MOVE PARTITION ... TO VOLUME 'cold'` | 2 skipped |
| Single DELETE | Prefix-scoped singular deletion log evidence | 2 skipped |
| Batch DELETE | Prefix-scoped plural deletion log evidence plus no unsupported-operation fallback | 2 skipped |
| Metadata and ordinary ETag | Cold/warm Parquet metadata-cache key is present, stable, and non-numeric | 2 skipped |
| Multipart | Create, part, and completion `ProfileEvents` all increase | 2 skipped |
| Refused XML request | Typed GCS XML error remains parseable on `gcs_hmac` | 1 skipped |

The CAS baseline separately requires every non-empty recorded incarnation token to be numeric, which
characterizes the GCS generation domain. The ordinary metadata-cache assertion requires a non-numeric
ETag. Those checks retain only booleans or opaque one-way digests outside traceback-hidden helpers.
Because all live cases skipped, neither provider value-domain observation was made in this run.

Outbound headers remain encrypted on the public endpoint and are not logged by this fixture. The
deterministic request-object tests pin the exact header partition; this lane's distinct responsibility
is whether Google accepts the resulting wire format. That acceptance remains unproven here.

## Exact lane results {#exact-lane-results}

The lanes ran strictly sequentially and retained `ci/tmp`.

| Lane | Praktika job | Pytest runtime | Counts | Skips | Result |
|---|---|---:|---|---|---|
| `test_gcs_live` | `Integration tests (amd_tsan, 1/6)` | 1.19 s | 2 passed, 0 failed, 25 skipped | 11 `gcs_hmac` credential; 10 `gcp_oauth` credential; 4 TLS fault-driver prerequisites | **Credential-free regressions passed; blocked: no live GCS acceptance** |
| `test_cas_s3` | `Integration tests (amd_tsan, 1/6)` | 27.51 s | 2 passed, 0 failed | 0 | Passed |
| `test_storage_s3` | `Integration tests (amd_tsan, 1/6)` | 94.46 s | 9 passed, 81 setup errors | 0 | **External image blocker** |

Commands and retained logs:

```bash
python3 -m ci.praktika run "integration" --test test_gcs_live > build/task10_gcs_live.log 2>&1
python3 -m ci.praktika run "integration" --test test_cas_s3 > build/task10_cas_s3_live.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_s3 > build/task10_storage_s3.log 2>&1
```

The ordinary S3 wrapper exited zero and printed an `Ok` summary despite the nested pytest result. The
nested result is authoritative here: 81 tests failed during setup because Docker reported the image
`clickhouse/clickhouse-server:23.3.19.33.altinitystable` as `manifest unknown`. The nine tests that did
not require that fixture passed. This is an external image-availability blocker and requires a CI
lane with the image available; it is not a green ordinary-S3 gate.

## Release follow-up {#release-follow-up}

Before release, rerun the same three commands with both live authentication sources and the ambiguity
driver available. Require all 25 real-GCS cases to execute with zero skips, rerun ordinary S3 where
the historical image resolves, inspect each test count and skip, and update this document with those
provider observations. Until then, deterministic coverage and the passing RustFS CAS lane do not
authorize a release claim.
