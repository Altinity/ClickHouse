---
description: 'The object-store contract a bucket must satisfy to host content-addressed storage, and which providers qualify.'
sidebar_label: 'Bucket requirements'
sidebar_position: 4
slug: /antalya/cas/bucket-requirements
title: 'CAS Bucket Requirements'
doc_type: 'reference'
---

# Bucket requirements {#bucket-requirements}

`CAS` is built on a small object-store contract (`Backend/CasBackend.h`), checked by a capability
probe that runs at every writable mount and fails closed: an object store that does not enforce
these conditions is refused rather than trusted.

## The capability table {#capability-table}

| Requirement | Interface method | Why it is needed |
|---|---|---|
| Read-after-write on a fresh key | `Backend::get` / `Backend::head` | Recovery listings and point reads must see what was just written |
| Conditional create (`If-None-Match: *`) | `Backend::putIfAbsent`, `Backend::casPut` with expected absence | Write-once creation of manifests and control/log objects; blob bodies use unconditional publication after `HEAD` |
| Conditional overwrite (`If-Match: <token>`) | `Backend::putOverwrite`, `Backend::casPut` | The one mutual-exclusion primitive: mount leases, `gc/state` |
| Unconditional complete-object publication | `Backend::publishBlob` | An absent or condemned content-addressed body is replaced atomically; native stores may use multipart |
| Native same-store copy when `cas_staging_backend = s3` | `IObjectStorage::copyObject` with `ObjectStorageCopyMode::NativeOnly` | The first absent staged publication may copy its complete object without a client-side fallback |
| Exact-token delete | `Backend::remove` | GC must delete only the incarnation it condemned, never a replacement |
| Ranged same-store copy, for backups only | `UploadPartCopy` through `copyS3File` | A `BACKUP` to an `S3(...)` destination on the pool's own endpoint copies a blob's payload without its header. Optional, but only when the lack is recognized: `GCS` lacks it by its current XML API, so `Client::supportsMultiPartCopy` reports `false` there and the copy goes through the ClickHouse server, which is slower but correct. A store that instead refuses `UploadPartCopy` with an error other than `AccessDenied` fails the `BACKUP`; set `s3_allow_multipart_copy = 0` there |
| Ranged `GET` | `Backend::get` / `Backend::getStream` with a `Range` | Opening one column file of a part costs one bounded read, not a whole-object fetch |
| `LIST` with a resumable cursor | `Backend::list` | GC discovery and the orphan-manifest sweep page through the pool without a separate index |
| No versioning / no delete markers | probed by `runCapabilityProbe`; `created_delete_marker` on `DeleteOutcome` | A delete marker over a live key would break exact-token semantics — GC would archive instead of reclaim |
| `TOKEN ⟹ CONTENT` (a repeated token implies unchanged bytes) | standing requirement on every `Backend` implementation | Not probed — it cannot be tested cheaply. A backend that recycled tokens would serve stale manifests, i.e. wrong query results, not merely an inefficiency |
| A body token never recurs at a key | standing requirement on the store | Not probed. A deletion intent and an exact-token delete both name one body incarnation; a store that gave a later body an earlier body's token would let a writer adopt a body `GC` is deleting. A token must also canonicalise to the pool's recorded dialect — the probe checks that the token of its own scratch object does and refuses the writable mount, pool creation and `cas-migrate-pool` otherwise |
| `GET` of an absent key answers `404` | `Backend::get` on `blob-intents/` | Every blob reuse reads the intent of the body it found; `404` means "no intent exists for this incarnation now"; the writer adopts the body because its precommit edge was durable before the `HEAD`. Any other answer, including `403`, fails the part write; see [deletion intents](#deletion-intents) |

A writable S3 mount also probes conditional batch deletion. It sends one `DeleteObjects`
with four scratch keys and an individual `ETag` for each. The probe checks the per-key results
for two matches, a stale `ETag` and an absent key, then verifies that both matching objects
disappeared and the replacement survived. GC uses conditional `DeleteObject` for each blob
when this capability is unavailable or the probe is skipped. GC can still use ordinary
`DeleteObjects` requests to clean up `blob-intents/` on stores that support them.

Bucket **versioning is not required** — in fact it must be **disabled** on the generation-token
dialect (see below), because a token-exact delete on a versioned bucket archives a noncurrent
generation instead of reclaiming storage, silently stopping GC reclamation.

On the generation-token dialect that requirement is checked at mount. A bucket reported as versioned
refuses the mount. A probe that could not answer — the credential may not read the bucket's versioning
configuration (`storage.buckets.get` on GCS), or the backend cannot report it — does not: the mount
proceeds and logs a warning naming what it could not verify, because an unreadable configuration is
not evidence of a versioned bucket, and refusing on it would turn a missing IAM grant into an outage.
In that case confirming that versioning is disabled is your responsibility, exactly as soft delete is
below; grant the permission if you want the mount to verify it for you. On a writable mount the SDK does
not retry the probe, and the probe runs under the same per-attempt budget as the mount's other
control-plane requests (`cas_attempt_timeout_ms`). A probe that timed out counts as one that could not
answer. A writable mount runs the probe once per start of the disk; a remount after a lost lease does
not repeat it.

Because that check is part of the mount battery, `skip_access_check = true` is refused on a writable
generation-token disk. Mount the disk read-only if you need to start before the access check can
pass.

**Incomplete multipart uploads.** With `cas_staging_backend = s3`, a process killed during a
multipart upload leaves an incomplete upload under `staging/<server_root_id>/`. Object `LIST` does
not show it, so neither the transaction's own delete nor the mount-start sweep can remove it, and its
parts stay billed. Add an `AbortIncompleteMultipartUpload` lifecycle rule on the bucket (one day is
enough: a staging upload completes within its own transaction or never).

## Deletion intents {#deletion-intents}

`blob-intents/` holds one small object per condemned body incarnation (see the
[blob protocol](/antalya/cas/architecture/blob-protocol#deletion-intent)). The credentials of a server
that mounts the pool need these rights on that prefix:

| Operation | S3 permission | Who | Notes |
|---|---|---|---|
| `GET` of a key that does not exist must answer `404` | `s3:GetObject`, and `s3:ListBucket` on the bucket | every writer | On S3 a missing key answers `404` only when the credential may also list the bucket; without it the store answers `403` and every reuse of an existing blob fails |
| `PUT` with `If-None-Match: *` | `s3:PutObject` | the `GC` leader | Creates the intent. An existing intent with the same bytes counts as created |
| `GET` and `HEAD` | `s3:GetObject` | `cas-fsck` | One `GET` of the intent of each `pending` `GC` row, and a `HEAD` of its body when the intent is absent. With `--verify-intents`, one `GET` of each listed intent and one `HEAD` of the body a well-formed intent names. `cas-fsck` never writes or deletes under `blob-intents/` |
| `LIST` | `s3:ListBucket` | the `GC` leader, `cas-fsck` | One page per `GC` round walks `blob-intents/` to find intents with no `GC` row. `cas-fsck` lists the whole prefix |
| `DELETE`, single and batch (`DeleteObjects`) | `s3:DeleteObject` | the `GC` leader | A refusal fails every round that must delete an intent |

A writable mount, pool creation and `cas-migrate-pool` check these rights before they write; the probe and the
message for each missing right are under
[rights on `blob-intents/`](/antalya/cas/operations/migration#migrate-generation-4-rights).

A pool uses exactly one token dialect (`etag`, `generation` or `emulated`), recorded in `_pool_meta` when
the pool reaches generation 4. Every client of the pool must be configured to report the same dialect;
a client whose dialect differs is refused at open.

## Soft delete is an operator precondition {#soft-delete}

Object **soft delete must be disabled** on a `CAS` bucket, and unlike versioning this one is *not*
verified at mount. Google Cloud Storage exposes the soft-delete policy through its JSON API, while
this backend and both of its authentication modes speak the XML API, so the storage path `CAS` uses
cannot inspect it. Disabling it is therefore your responsibility, not something a successful mount
attests to.

Soft delete does not leave the deleted generation live, so it does not break exact-token semantics
the way versioning does. What it does is delay physical reclamation until the retention period
expires: `GC` reports space as reclaimed while the bill still reflects it.

## Request rate, and the limit that is not the one you expect {#request-rate}

Google Cloud Storage publishes two kinds of ceiling, and the one that constrains `CAS` is the
smaller and less-known of them.

A bucket starts at roughly **1000 object writes per second** — uploads, updates and deletes — and
roughly **5000 object reads per second**, counting listings and metadata reads as reads. Those
ceilings are not fixed: Cloud Storage raises them by splitting the index range behind the bucket,
which it says takes "on the order of minutes" to detect and act on. Buckets with a hierarchical
namespace start up to eight times higher.

Separately, Cloud Storage applies **a much smaller limit to repeated writes to the same object
name**. Google documents that this limit exists but does not publish its value. Measured against a
live bucket from this codebase, it begins to bite at approximately one mutation per second on a
single key, and it does not participate in the auto-scaling above — splitting an index range cannot
help a single name.

That second limit is the one `CAS` meets first, because two of its objects are single fixed names:

| Object | One per | Written on |
|---|---|---|
| `cas/ref_catalog` | pool | twice per `CREATE TABLE` and twice per `DROP TABLE` |
| `cas/ns/state/<namespace>/_ckpt` | table | namespace creation, each snapshot publication, and once when a namespace first receives a generation 3 write (the upgrade stamp). No ref-log transaction writes it |

A ref-log transaction is one create of its own key, `_log/<epoch>-<sequence>`, so a commit never repeats
a write to a name. Only the catalog is written often enough to meet the per-name limit.

Blob bodies and deletion intents are named by content hash and are therefore spread the way
Google's own guidance asks for: it recommends "completely random object names" for the best load
distribution, and a hashed prefix where names would otherwise be sequential. Ref-log transactions are
sequential within a namespace but are written under a per-namespace prefix, so they scale with the
number of tables rather than sharing one index range.

### What this means for a deployment {#rate-consequences}

- **A table's commit rate is not capped by the per-name limit.** Each transaction creates a new key,
  so inserts, merges and mutations on one table do not queue behind a repeated write to one name.
  Snapshot publication writes `_ckpt` once per snapshot.
- **A pool performs about one table lifecycle transition per second.** Concurrent `CREATE TABLE` or
  `DROP TABLE` beyond that rate contends on the pool-wide catalog. Test suites that create hundreds
  of tables in parallel are the case that provokes this; ordinary production DDL is not.
- **Ramping up gradually is Google's documented expectation.** Its guidance is to increase the
  request rate "no faster than doubling the rate over a period of 20 minutes", and to pause or
  reduce the rate when latency or error rates rise. A pool that goes from idle to full write load in
  one step will see throttling before the bucket has redistributed the load.

### Throttling is a retryable condition, not a failure {#rate-errors}

Cloud Storage signals a rate it will not serve with HTTP `429`, `408`, or a `5xx` status, and its
retry guidance names all three, together with socket timeouts and TCP disconnects, as retryable with
exponential backoff and jitter. Every mutable-object write `CAS` issues carries a generation
precondition, which places it in Google's *conditionally idempotent* class — a retry either applies
exactly once or fails the precondition, never applies twice. Retrying them is therefore safe by
Google's own rule, not merely by ours.

### Reads over a wide-area link want a cache disk {#rate-reads}

The read ceiling is high enough that `CAS` does not approach it, but latency is a separate matter.
A cacheless `CAS` disk pays a round trip per column file per part: measured against a bucket in
another region, a `SELECT` issued about 725 ranged reads and took 3.6 seconds at the median and 15.7
seconds at the ninety-ninth percentile. Put a `cache` disk in front of the `CAS` disk for any
deployment where the bucket is not local to the server.

## Platform support {#platform-support}

The deterministic request-construction coverage is green, but the
real-GCS release gate remains blocked
until its credentialed OAuth and HMAC groups run against Google Cloud Storage. A fake service cannot
establish acceptance of Google's multipart, native-copy, and exact-delete wire behavior.

| Platform | Status | Notes |
|---|---|---|
| AWS S3 | ✓ | Native `ETag`-based conditional dialect for mutable objects and exact deletion; blob publication is unconditional |
| Google Cloud Storage | implementation complete; release gate pending | Generation-token dialect for mutable objects/native-token `HEAD`/exact deletion, opted into via `http_client = gcs_hmac` or `gcp_oauth`; blob publication uses ordinary copy/multipart. Real credentialed GCS groups have not run yet |
| Azure Blob Storage | probably | Azure's REST API documents the equivalent conditional headers, but ClickHouse's Azure object-storage backend does not yet wire up a `CAS` conditional dialect the way the S3 and GCS paths do — untested, not validated by the capability probe |
| Other S3-compatible stores | only with enforced conditional operations | A store that silently ignores required `If-None-Match` or `If-Match` conditions fails the mount probe. RustFS 1.0.6 passes both the required single-key checks and the optional per-object `ETag` check for `DeleteObjects`. Garage was evaluated and rejected because it silently ignores conditional operations. |

The full mechanics of the two dialects — how the backend detects which one a given endpoint speaks,
what the capability probe actually checks, and how exact-token deletes map onto each provider's
primitives — are in [the Backend architecture page](/antalya/cas/architecture/backend).
