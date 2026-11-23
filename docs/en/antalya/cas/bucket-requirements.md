---
description: 'The object-store contract a bucket must satisfy to host content-addressed storage, what must not run on it, and which providers qualify.'
sidebar_label: 'Bucket requirements'
sidebar_position: 4
slug: /antalya/cas/bucket-requirements
title: 'CAS Bucket Requirements'
doc_type: 'reference'
---

# Bucket requirements {#bucket-requirements}

`CAS` is built on a small object-store contract (`Backend/CasBackend.h`), checked by a capability
probe that runs when a writable disk starts (unless `skip_access_check` skips it) and fails closed: an object store that does not enforce
these conditions is refused rather than trusted.

## The capability table {#capability-table}

| Requirement | Interface method | Why it is needed |
|---|---|---|
| Read-after-write on a fresh key | `Backend::get` / `Backend::head` | Recovery listings and point reads must see what was just written |
| Conditional create (`If-None-Match: *`) | `Backend::putIfAbsent`, `Backend::casPut` with expected absence | Write-once creation of manifests and control/log objects; blob bodies use unconditional publication after `HEAD` |
| Conditional overwrite (`If-Match: <token>`) | `Backend::putOverwrite`, `Backend::casPut` | The one mutual-exclusion primitive: mount leases, `gc/state` |
| Unconditional complete-object publication | `Backend::publishBlob` | An absent or condemned content-addressed body is replaced atomically; native stores may use multipart |
| Native same-store copy when `cas_staging_backend = s3` | `IObjectStorage::copyObject` with `ObjectStorageCopyMode::NativeOnly` | The first absent staged publication may copy its complete object without a client-side fallback |
| Exact-token delete | `Backend::deleteExact` | GC must delete only the incarnation it condemned, never a replacement |
| Ranged same-store copy, for backups only | `UploadPartCopy` through `copyS3File` | A `BACKUP` to an `S3(...)` destination on the pool's own endpoint copies a blob's payload without its header. Optional, but only when the lack is recognized: `GCS` lacks it by its current XML API, so `Client::supportsMultiPartCopy` reports `false` there and the copy goes through the ClickHouse server, which is slower but correct. A store that instead refuses `UploadPartCopy` with an error other than `AccessDenied` fails the `BACKUP`; set `s3_allow_multipart_copy = 0` there |
| Ranged `GET` | `Backend::get` / `Backend::getStream` with a `Range` | Opening one column file of a part costs one bounded read, not a whole-object fetch |
| `LIST` with a resumable cursor | `Backend::list` | GC discovery and the orphan-manifest sweep page through the pool without a separate index |
| No versioning / no delete markers | probed by the capability probe, which refuses a delete that created a delete marker | A delete marker over a live key would break exact-token semantics — GC would archive instead of reclaim |
| `TOKEN ⟹ CONTENT` (a repeated token implies unchanged bytes) | standing requirement on every `Backend` implementation | Not probed — it cannot be tested cheaply. A backend that recycled tokens would serve stale manifests, i.e. wrong query results, not merely an inefficiency |
| A body token never recurs at a key | standing requirement on the store | Not probed. A deletion intent and an exact-token delete both name one body incarnation; a store that gave a later body an earlier body's token would let a writer adopt a body `GC` is deleting. A token must also canonicalise to the pool's recorded dialect — the probe checks that the token of its own scratch object does and refuses the writable mount, pool creation and `cas-migrate-pool` otherwise |
| `GET` of an absent key answers `404` | `Backend::get` on `blob-intents/` | Every blob reuse reads the intent of the body it found; `404` means "no intent exists for this incarnation now"; the writer adopts the body because its precommit edge was durable before the `HEAD`. Any other answer, including `403`, fails the part write; see [deletion intents](#deletion-intents) |

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

A writable mount and pool creation (unless the disk sets `skip_access_check`) and `cas-migrate-pool` check these rights before they write; the probe and the
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

## What must not run on the bucket {#must-not}

`CAS` cannot see the policies that apply to a pool's keys. You provision them and verify them.

An ordinary writable start runs the capability battery, unless the disk sets `skip_access_check`. A writable generation-token disk refuses that setting. The battery writes and deletes only scratch keys, under `_probe/` and `blob-intents/_probe/`. Except on local object storage, it also lists `blob-intents/` itself. The mount reads no lifecycle, retention, replication or access policy. The one setting it queries is bucket versioning, and only on the generation-token dialect (see the paragraphs after the [capability table](#capability-table)). A rule that covers other keys, or that starts after the mount, goes unseen.

Provisioning rules:

- Give each pool one authoritative bucket and a prefix that overlaps no other pool's prefix: no shared, parent or child prefix.
- Verify the effective bucket and prefix policies before you provision the pool, and again after every policy change.
- Let only the pool's servers and the `clickhouse-disks` commands in this documentation write or delete under the prefix.
- Exclude completed pool objects from expiration, from Object Lock and other WORM retention and legal holds, from storage-class transitions that make an object unreadable without a restore, and from replication or sync into the prefix.

The only exclusivity check runs when a pool is created. A writable start refuses to create a pool at a prefix that already holds other objects (`missing _pool_meta over a non-empty pool prefix`), except probe scratch and the empty catalog. The check is skipped when `_pool_meta` is readable. It lists only keys below the configured prefix. Creating `p` after `p/x` is refused. Creating `p/x` after `p` is not checked.

What breaks:

- **Expiration** (a lifecycle rule by age, tag or prefix). The one lifecycle action a pool bucket needs is `AbortIncompleteMultipartUpload`, above. By what is gone:
  - A manifest that a committed reference names: an uncached read fails with `FILE_DOESNT_EXIST` and `live ref names manifest at <key> but its object is missing`. A cached decode is served until it is evicted.
  - A blob body that a committed reference names (every file that is not inline in the manifest): a read that needs a range of it fails. With a `type=cache` disk, a range already in the filesystem cache is served without contacting the store, so a query that succeeds on a warm replica does not show that the object still exists.
  - `cas-fsck` reports either as `dangling`, and `clickhouse-disks --query 'cas-fsck'` exits with 1. It confirms a missing object by `HEAD` and by re-reading the refs. It does not re-check a blob that its own listing shows, so a deletion after the listing can escape that scan.
  - A fold seal that `gc/state` has adopted, or a run the round needs: the loss can stop `GC` reclaiming. See [GC refuses its bookkeeping](/antalya/cas/operations/debugging#gc-bookkeeping-corrupt).
  - `_pool_meta`: the next writable start over the remaining objects, and any read-only open, are refused with `INVALID_STATE`.
- **A storage class that cannot serve a read at once** (one that needs a restore first). If `HEAD` still answers for such an object, a later insert of the same content reuses its body without reading it. That needs a matching logical size, a canonical token and no deletion intent. `CAS` never takes a read error other than not-found as absence, so an operation that must read the object fails with the store's error. A scheduled `GC` round that fails retries on the next tick.
- **A store that refuses the server's deletes** (for example an Object Lock or retention policy, or a legal hold, on pool objects). A refused delete of a condemned body fails that `GC` round before its commit, and the row stays pending. Deletes sent earlier in the same batch may already have freed space. The log shows `pending delete of blob <hash> ... failed; the round fails before its commit and the row stays pending`. A scheduled round that fails also logs `CA GC round failed (will retry next tick)`. A refused delete of an intent is described under [deletion intents](#deletion-intents). A rule that refuses the deletes of the probe's scratch keys fails the mount with the store's error.
- **Versioning** (see [versioning](#capability-table) and [soft delete](#soft-delete)). A delete then archives a noncurrent version and frees nothing. The rule differs by dialect:
  - Plain S3 (the `etag` dialect, such as AWS S3): the mount does not query the setting. It refuses when the probe's delete response reports a delete marker (`CasProbe: remove succeeded but created a versioning delete marker`). Suspending versioning does not help, because deletes on a suspended bucket still create markers. Provision a bucket where versioning was never enabled.
  - Generation-token dialect: the mount queries the setting. It refuses a bucket reported as versioned, and warns and proceeds when the query gets no answer. The probe's marker check applies here too.
  - After the mount, on either dialect, an exact delete whose response reports a marker fails with `CAS_DELETE_MARKER` and fails the `GC` round. A store that reports no marker, and is not queried, goes undetected.
- **A copy of the prefix made object by object** (bucket replication, a sync), mounted writable or read-only. The copy carries the same `pool_id` and `blob_header_len`, the only two fields the pool identity check compares. A remount also checks the catalog and the owner. A part is published as manifest, precommit entry, blobs, promotion entry. A copy follows no such order, so it can hold a promotion entry whose blob is missing, and reads of that part fail as above (when they need a range that is not cached). For a second copy of the data, use `BACKUP` to a destination outside the pool prefix. Replication between the servers of one pool is not affected: a successful same-pool relink reuses the blob bodies, and the receiver writes the metadata.
- **Another writer under the prefix** (a second pool, a replication job, a sync script). A foreign body under a blob key is reused unread by a later insert of the same content, when its logical size matches, its token is canonical and no deletion intent exists. A body of another size fails the insert with `CORRUPTED_DATA` and `has logical size <n>, expected <m>`. A body shorter than the blob envelope fails it with `CORRUPTED_DATA` and `is below envelope length`.

Removing pool objects outside the protocol, by a lifecycle rule or by hand, is unsupported. `CAS` cannot identify such a deletion as the cause of a loss, and does not repair it.

## The bucket credential is the trust boundary {#trust-boundary}

A principal can overwrite the keys it may write and remove the keys it may delete, and `CAS` cannot tell it from a pool member. Treat pool-wide mutation credentials like cluster administrator credentials. Grant them only to the pool's servers and to the administrators who run the commands below.

- **No writer identity.** The control objects (owner, epoch, mount lease, `gc/state`) have no signature or writer field. A principal that may write them can retire a member (`retired_at_ms` on an owner object), fence its writes (`gc_fenced` on a mount lease) or write a mount lease to claim its slot.
- **Delete rights.** A principal that may delete under the prefix can remove the blobs, manifests and refs of tables it does not own. Write rights on one subtree do not imply delete rights across the pool: grant each separately.
- **Servers.** Rights under the prefix, by operation:
  - Read and list, for every server: `GET` and `HEAD` (`s3:GetObject`) and `LIST` (`s3:ListBucket`). Opening a pool reads `_pool_meta`, and a writable mount reads back the probe's scratch objects.
  - Create, overwrite and delete (`s3:PutObject`, `s3:DeleteObject`), for writable servers only. The mount tests them on scratch keys only. A writable disk with `cas_gc_enabled` on (the default) can become the `GC` leader, and the leader deletes objects that other servers wrote. A server that passes the mount but may not delete pool objects fails the rounds it leads.
  - A read-only server needs no write right and should not get pool-wide delete rights.
  - Whether a particular IAM policy suffices was not exercised.
- **Read access.** Part files are stored as blobs behind a constant-size header, and `CAS` adds no encryption of its own: encryption at rest has to come from the object store. A principal that can read the prefix can read the table data.
- **Readers outside the servers.** Give backup, log-shipping and analytics readers only the read and list rights they need on the source prefix. `cas-fsck`, `cas-inspect` and `cas-gc-dryrun` need a disk with `<readonly>true</readonly>`. A read-only open mints no pool metadata, runs no probe and claims no mount slot, and these tools issue no write requests. Three commands write through such a disk on purpose: `cas-gc-rebuild`, `cas-drop-member` and `cas-fsck --fix legacy-markers`. On a local object storage, `cas-fsck --fix legacy-markers` is refused, because the read-only flag blocks its deletes: run `cas-migrate-pool` instead, which removes the legacy markers of a migrated pool. `cas-migrate-pool` also writes, and its disk need not be read-only. Whether a policy limited to read rights suffices for the read tools was not exercised against a real store.

SQL users who insert through the servers do not hold the bucket credential, and ordinary inserts do not give them its authority. A user can still act with that authority without knowing it, when a storage integration that SQL can reach uses the same credential as the pool: an `s3` table function or `S3` table, an S3 named collection, or another disk. Server-managed credentials from the environment are closed to user queries unless `s3_allow_server_credentials_in_user_queries` is on. Static keys in the server `<s3>` configuration or in a named collection are not. Keep the pool's write credential out of every such integration. The `sha256` hash choice for mutually untrusted users ([blob protocol](/antalya/cas/architecture/blob-protocol)) concerns hash collisions between inserted data. It does not limit what a credential can do.

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
| Other S3-compatible stores | only with enforced conditional operations | The capability probe is the actual gate: a store that silently ignores `If-None-Match`/`If-Match` (accepting and applying the write regardless) fails the probe and is refused. `RustFS` passes the full battery and is used as the project's test backend; `Garage` was evaluated and rejected because it silently ignores conditional operations |

The full mechanics of the two dialects — how the backend detects which one a given endpoint speaks,
what the capability probe actually checks, and how exact-token deletes map onto each provider's
primitives — are in [the Backend architecture page](/antalya/cas/architecture/backend).
