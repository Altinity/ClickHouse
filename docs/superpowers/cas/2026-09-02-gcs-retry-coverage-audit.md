---
description: 'Which CAS object reads and writes are covered by an automatic retry, which are covered only by the object-storage client, and which are not retried at all, with the consequence of each gap on Google Cloud Storage'
sidebar_label: 'GCS retry-coverage audit'
sidebar_position: 1
slug: /superpowers/cas/gcs-retry-coverage-audit-2026-09-02
title: 'CAS retry-coverage audit (2026-09-02)'
doc_type: 'reference'
---

# CAS retry-coverage audit — which object reads and writes are not covered by a correct automatic retry {#cas-retry-coverage-audit-which-object-reads-and-writes-are-n}

Repository `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`.
Subsystem `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.
Read-only investigation: nothing was edited, built, run, or deployed.

Paths below are relative to the subsystem directory unless prefixed with `src/`.
Call sites are cited by enclosing symbol first; line numbers are as of this tree and will move.

---

## 1. The controlled path, in two sentences {#1-the-controlled-path-in-two-sentences}

`DB::Cas::CasRequestController` (`Backend/CasRequestControl.h`, `.cpp`) is the subsystem's own
request controller: for one logical conditional write it issues up to `budget.max_attempts`
(default 16) attempts of the *identical* `(key, bytes, expected token)` inside
`budget.operation_deadline_ms` (default 90 s), with capped-exponential backoff (200 ms doubling to
5 s), a mount-fence check before every attempt and before every sleep, and — the load-bearing part —
a **resolve-by-exact-GET of any ambiguous attempt before it is reissued**, so an uncertain PUT is
never blindly repeated and a lost response is never turned into a false failure.

Its verdict is the *call's*, not the last attempt's: `DefiniteFailure` is returned only when every
attempt this call sent was itself proven never applied, and `classifyConditionalWriteResult`
(`src/IO/S3Common.cpp`) whitelists only malformed-request, entity-too-large and access-denied as
definite — so **a 429, a 5xx or a timeout can never reach a definite-failure verdict**; it is always
`Unresolved`, which the callers treat as "retry later", never as "the key is free".

Its five entry points are `putIfAbsentControlled`, `putOverwriteControlled`,
`putIfAbsentControlledMutable`, `resolveByExactGet` and `slotOccupy`. `slotOccupy` is deliberately
**not** a retry loop: it is one fence-gated conditional create plus at most one resolve GET, and the
retry discipline belongs to its caller.

### The two layers underneath it {#the-two-layers-underneath-it}

**State 2 — the object-storage client's own retries.** ClickHouse's S3 client
(`Client::RetryStrategy::ShouldRetry`, `src/IO/S3/Client.cpp`) retries whatever the AWS SDK marks
retryable, up to `max_retries` = server setting `s3_retry_attempts`, **default 500**
(`src/IO/S3Defines.h`, `DEFAULT_RETRY_ATTEMPTS`), with backoff 25 ms doubling to a 5 s cap and no
jitter. The SDK marks `429 TOO_MANY_REQUESTS`, `408 REQUEST_TIMEOUT`, 500, 502, 503 and 504
retryable (`contrib/aws/.../http/HttpResponse.h`, `IsRetryableHttpResponseCode`). ClickHouse
un-retries only `301` and `412 PreconditionFailed`. **So yes — a 429 is retried, and heavily, on any
CAS request that uses the default client.** Reads, HEADs, LISTs, `deleteExact` and `publishBlob` all
do.

**State 3 — no retry at any layer.** `ObjectStorageBackend::conditionalWriteSettings`
(`Backend/CasObjectStorageBackend.cpp`) sets `ObjectStorageRetryProfile::SingleAttempt` and
`s3_max_unexpected_write_error_retries_override = 1`; `S3ObjectStorage::writeObject` then selects
`getSingleAttemptClient()`, whose `SingleAttemptRetryStrategy::ShouldRetry` always returns false.
**Every Native-mode `Backend::putIfAbsent`, `Backend::putOverwrite` and `Backend::casPut` therefore
makes exactly one HTTP attempt.** `detail::finalizeConditionalWrite` maps only a 412 and a 404
`NoSuchKey` to a `PreconditionFailed` outcome; a 429, a 5xx or a timeout is **rethrown on the first
failure**. Any direct call to those three methods that is not wrapped in `CasRequestController` is
state 3.

This is by design and it is right: a transparently retried conditional write can cross the mount
lease boundary. The defect is not the single-attempt profile, it is the **twenty-three production
call sites that use it without a controller above them**.

### The one safety property that holds everywhere {#the-one-safety-property-that-holds-everywhere}

A throttling or transport error **cannot** be read as "the object is absent". In Native mode
`ObjectStorageBackend::head` → `S3::getObjectInfoIfExists` (`src/IO/S3/getObjectInfo.cpp`) returns
an empty result only for `isNotFoundError` — `NO_SUCH_KEY`, `RESOURCE_NOT_FOUND`, `NO_SUCH_BUCKET` —
and throws everything else; `Backend::get` uses `isObjectNotFound`, which matches only
`NO_SUCH_KEY` / `"NoSuchKey"` / `FILE_DOESNT_EXIST`. Every `!exists` and every `nullopt` in the
subsystem is therefore reachable only from a genuine 404. I looked for the "retryable error
classified into a decision" hazard everywhere and **did not find it**: no delete, detach, wedge,
demotion, takeover or lifecycle transition in this subsystem can be triggered by a 429 or a 5xx.
That negative result is the most important finding in this report, and it is what keeps every gap
below in the availability class rather than the data-loss class.

Note that the `ProbeOutcome` doc comment in `Backend/CasBackend.h` says the opposite — that `head`
and `get` flatten "a permission failure, a transport fault" into not-found. That is false for the
S3 backend and should be corrected, but the code is right and the comment is wrong, not the reverse.

---

## 2. Object kinds and their coverage {#2-object-kinds-and-their-coverage}

21 object kinds. `<p>` is the pool prefix.

| # | object kind | key shape (`Formats/CasLayout.h`) | operations | coverage |
|---|---|---|---|---|
| 1 | blob body | `blobKey` = `<p>/blobs/<algo>/<shard>/<hex>` | HEAD, publish (PUT/COPY), GET ranged, `deleteExact` | **2** everywhere; publish additionally wrapped in an 8-attempt re-HEAD ambiguity loop in `PartWriteTxn::ensureBlobPresent` |
| 2 | blob freshness-meta sidecar | `blobMetaKey` = `<blobKey>.meta` | GET, PUT if-absent, CAS, `deleteExact` | **1** for both writes (`putIfAbsentControlledMutable`, `putOverwriteControlled`); **2** for GET and delete |
| 3 | part manifest | `manifestKey` = `<p>/cas/manifests/<ns>/<epoch-seq>/<ordinal>.zst` | PUT if-absent, GET, HEAD, `deleteExact` | **1** for the stage (`PartWriteTxn::stageManifest`); **2** for reads and cleanup deletes |
| 4 | ref-log transaction | `refLogKey` = `<p>/cas/ns/stream/<life>/_log/<id>.zst` | PUT if-absent, GET | **1** in `commitRefChunk`; **3 + one GET** for the two `slotOccupy` sites (epoch seal, wedge retry), each with a caller-level retry loop |
| 5 | ref snapshot | `refSnapshotKey` = `.../_snap/<id>.zst` | PUT if-absent, GET | **1** |
| 6 | per-namespace checkpoint | `refCkptKey` = `<p>/cas/ns/state/<life>/_ckpt` | CAS put, GET | **3 per attempt**, inside a caller loop bounded by 100 attempts and a deadline, resolve-by-GET before every reissue, **no backoff** |
| 7 | pool-wide reference catalog | `refCatalogKey` = `<p>/cas/ref_catalog` | PUT if-absent, CAS put, GET | **3**, uncontrolled, uncaught — see Gap 1 |
| 8 | pool meta / pool identity | `poolMetaKey` | CAS put, GET, sentinel probe | **3** for both CAS sites; **2** for reads |
| 9 | server-root owner | `ownerKey(srid)` | PUT if-absent, GET, tombstone overwrite | **3** for the claim; **1** for the decommission tombstone |
| 10 | server writer epoch | `epochKey(srid)` | CAS put, GET | **3**, on every mount and every remount |
| 11 | mount lease | `mountKey(srid)` | PUT if-absent, 5× putOverwrite, GET, LIST | **1** for the renewal only; **3** for mint, refresh, reclaim, keeper adopt, GC fence-out and farewell |
| 12 | GC state | `gcStateKey` = `<p>/gc/state` | CAS put (acquire, renew, steal, round commit, rebuild), GET | **3** for all five writes |
| 13 | GC maintenance state | `gcMaintenanceStateKey` | CAS put, GET | **3** |
| 14 | GC heartbeat | `gcHbKey` = `<p>/gc/hb` | CAS put, GET | **3** |
| 15 | fold seal | `foldSealKey` = `<p>/gc/gen/<G>/attempt/<A>/fold_seal` | PUT if-absent, GET | **3** for the write |
| 16 | blob-target run | `blobTargetRunKey` | PUT if-absent, streamed GET | **3** for the write |
| 17 | GC outcomes log | `outcomesKey` | PUT if-absent, GET | **3** for the write |
| 18 | namespace file | `namespaceFileKey(life, name)` | HEAD then PUT if-absent / putOverwrite, GET, LIST, `deleteExact` | **3** for the writes (`CasPlainObjects::casPutObject`); **2** for the rest |
| 19 | mountpoint (loose) object | `mountpointObjectKey(key)` | same as above | **3** for the writes; **2** for the rest |
| 20 | S3 staging object (opt-in) | `<p>/staging/<mount_id>/<rand>.tmp` | PUT rewrite, GET, DELETE | **2** |
| 21 | capability-probe scratch | `<p>/_probe/<u128>/{token,cas}` | 8 conditional writes, ~9 GET, 2 LIST, 4 `deleteExact` | **3** for the 8 writes; **2** for the rest |

---

## 3. Gaps, ordered by severity {#3-gaps-ordered-by-severity}

### Gap 1 — CRITICAL. The pool-wide reference catalog is written twice per `CREATE TABLE`, single-attempt, uncaught {#gap-1-critical-the-pool-wide-reference-catalog-is-written-tw}

`CasRefCatalog::casUpdateImpl` (`Pool/CasRefCatalog.cpp`, ~line 127) calls `backend.casPut`
directly. Its loop retries **only** `CasOutcome::Conflict`, i.e. only a returned 412. A thrown 429,
5xx or timeout propagates out of the loop on the first attempt, and there is no `catch` anywhere
between it and the DDL entry point.

Write count per statement, traced through `CasRefCatalog::createNamespace`:

| statement | writes to `<p>/cas/ref_catalog` |
|---|---|
| `CREATE TABLE` | **2** — `createNamespaceStep1` inserts the `Creating` row, then `completeCreation` flips it to `Live` |
| `DROP TABLE` | 1 — `beginRemoving`, plus 1 later from GC's `deleteCompletedRemovingAtSnapshot` |
| `RENAME TABLE` | 1 or more via `casUpdate` |

The two `CREATE TABLE` writes are back-to-back on **one pool-wide key**, separated only by the
`_ckpt` publish, each a single HTTP attempt with no retry and no backoff. This is exactly the
"smaller limit for repeated writes to the same object name" that GCS enforces at roughly one
mutation per second per key. **This confirms the reported ~40 % `CREATE TABLE` failure rate at the
mechanism level**: two same-key mutations per statement against a ~1/s per-key ceiling, with the
provider's own retry advice ignored.

Consequence: `CREATE TABLE`, `DROP TABLE` and `RENAME TABLE` fail with `S3_ERROR`. Secondary: an
ambiguous failure at step 1 or step 3 leaves a `Creating` or `Live` row for a table UUID ClickHouse
never created; a retried `CREATE TABLE` mints a fresh UUID, so the row is debris until
`cancelStalledCreating` or GC clears it.

The fix shape is already present in the tree: `putOverwriteControlled` has exactly the semantics
this loop hand-rolls, including resolve-before-reissue, and `casUpdateImpl` is a single choke point
for every catalog mutation.

### Gap 2 — HIGH. A writable mount issues ~12 single-attempt conditional writes; one 429 refuses the mount {#gap-2-high-a-writable-mount-issues-12-single-attempt-conditi}

Per writable `Pool::open`, in order:

| step | site | conditional writes |
|---|---|---|
| capability battery | `runCapabilityProbe` (`Backend/CasProbe.cpp`) | **8** (`putIfAbsent` ×2, `putOverwrite` ×2, `casPut` ×4) |
| writer epoch | `allocateWriterEpoch` (`Pool/CasServerRoot.cpp`) | 1, every mount |
| mount lease claim | `claimMount` | 1, every mount |
| keeper adopt | `MountLeaseKeeper::claim` | 1, every mount |
| disk access check | `IDisk::checkAccessImpl` → `CasPlainObjects::casPutObject` | 1, every startup |
| fresh server root | `claimOwnerOrThrow` | +1 |
| fresh pool | `initializeEmptyForNewPool`, `PoolMeta::createOrValidate` | +2 |

All are state 3. A 429 on any one throws out of `Pool::open` →
`ContentAddressedMetadataStorage::startup` → `DiskObjectStorage::startupImpl` →
`DiskSelector::initialize`, which rethrows: **server startup aborts for a config-declared disk**, or
the `CREATE TABLE` fails for a `disk(...)`-defined one. Nothing retries.

The access-check write is not optional on the bucket type that throttles: `checkSkipAccessCheckSupport`
(`Backend/CasObjectStorageBackend.cpp`) *refuses* `skip_access_check` on a writable
generation-token (GCS) mount, so that path always runs. `IDisk::startup` catches, calls `shutdown()`
and rethrows.

A throttled probe is at least never *misjudged*: every "expected `PreconditionFailed`" arm in the
battery is reachable only from a returned 412, so a 429 escapes as a raw `S3Exception` rather than
being recorded as a failed capability. The mount is refused, not wrongly diagnosed.

### Gap 3 — HIGH. Loose table files and namespace files are written single-attempt {#gap-3-high-loose-table-files-and-namespace-files-are-written}

`CasPlainObjects::casPutObject` (`Pool/CasPlainObjects.cpp`) does HEAD → `backend.putIfAbsent` or
`backend.putOverwrite`, retrying only on `PreconditionFailed`. Reached from
`ContentAddressedTransaction::writeFile` (non-part branch), `moveFile`, `moveDirectory`, and the disk
access check. A single 429 surfaces as `S3_ERROR` to the user statement.

Consequences: `CREATE TABLE` fails on `format_version.txt` and similar table-level files;
`RENAME TABLE` fails mid-way and logs that the table is left split across namespaces (re-drivable by
repeating the rename, so not irreversible); the disk access check fails the mount, per Gap 2.

`casRemoveObject` in the same file is state 2 and fine.

### Gap 4 — HIGH. The `_ckpt` retry loop spins without backoff on a per-key-rate-limited singleton {#gap-4-high-the-ckpt-retry-loop-spins-without-backoff-on-a-pe}

`publishCkpt` (`Pool/CasRefCkpt.cpp`, ~line 306) is correct in the part that matters — it catches the
thrown CAS, re-reads the exact object, and only reissues against that fresh observation, never
against remembered bytes. What it does not do is **sleep**. Up to 100 iterations of PUT + GET run
back-to-back against the same key, bounded only by `MAX_CKPT_CAS_ATTEMPTS` and the `CkptDeadline`.

Against a store that is throttling *because* of the per-key mutation rate, this amplifies the
condition it is reacting to. `_ckpt` is written once per `CREATE TABLE` and once per committed ref
chunk, i.e. per `INSERT`, merge, `ALTER` and part drop. Exhaustion raises `NETWORK_ERROR`, which
fails the statement or moves the lane to `NeedsRecovery` — reversible, but recovery then pays the
same key again.

### Gap 5 — MEDIUM. Both GC liveness signals are single-attempt, so a throttling episode causes leadership churn {#gap-5-medium-both-gc-liveness-signals-are-single-attempt-so}

`Gc::pulseHeartbeat` (`Gc/CasGc.cpp`, ~4373) and the renew CAS in `Gc::acquireOrRenewLease` (~4418)
are both state 3. A follower steals when, across two of its own ticks, both `(lease.owner,
lease.seq)` and `(hb.owner, hb_seq)` are unchanged. A throttling burst lasting longer than one
scheduler interval fails both writes, freezes both signals, and the steal fires deterministically.

Safe but expensive: the deposed leader's fold lands under its own `attempt = lease.seq`, its round
CAS fails, and its pre-CAS actions (blob redeletes from the shared parent seal, generation prunes
below the shared floor) are the same ones the new leader would take. Every handover costs a full
re-fold, and under sustained throttling it can oscillate. The `CasGcScheduler::loop` catch comment
already names this loop; keeping `i_am_leader` on a transient error does not make the *write*
survive a 429.

### Gap 6 — MEDIUM. Any single throttled PUT discards an entire GC round {#gap-6-medium-any-single-throttled-put-discards-an-entire-gc}

Every GC-plane write in a round is state 3 with no round-local retry: the fold seal and every
blob-target run (`putDeterministicArtifact`, `Gc/CasBlobInDegree.cpp` ~335), the outcomes log
(`Gc::runRegularRound` ~843), the mount fence-out (`computeHeartbeatFloor` ~1215), and the round
commit CAS (~944). One throttled request anywhere throws away the whole fold; the next tick redoes
it from scratch under a fresh attempt.

Under persistent throttling GC completes no rounds at all, so reclamation stops and storage grows.
No wrong decision is taken — the attempt-scoped debris is reclaimed by the generation prune — but
this is the failure mode that silently costs money.

### Gap 7 — MEDIUM. The lease renewal is covered, but the recovery it triggers is not {#gap-7-medium-the-lease-renewal-is-covered-but-the-recovery-i}

The renewal itself is state 1 (`MountLeaseKeeper::renew` → `putOverwriteControlled`,
`Pool/CasServerRoot.cpp` ~1735), with an `ExternalLeaseSafety` deadline derived from the lease TTL.
At defaults (TTL 30 s, renew period 10 s, safety margin 2 s, attempt timeout 5 s, backoff 200 ms →
5 s) the attempt starts fall at 10.0, 10.2, 10.6, 11.4, 13.0, 16.2, 21.2 s after the anchor, and the
next cannot fit: **roughly 7 consecutive throttled attempts, about 11–13 s of sustained 429 on the
mount key, trips the fence.** The 16-attempt cap never binds.

Once tripped, `checkOpAdmitted` throws `NETWORK_ERROR` for every operation class except Factory, so
`SELECT` metadata reads fail alongside writes. It does not self-heal on the next tick: the keeper is
terminal until a remount installs a new one, and `Pool::tryRemountOnce` must re-run
`allocateWriterEpoch`, `claimMount` and the keeper adopt — **all state 3** — after a
token-stability observation of TTL + TTL/20 + poll ≈ 36.5 s. A throttled write anywhere in that
sequence returns false, backs off up to 30 s, and restarts the observation from zero. Minimum outage
after a trip is ~36.5 s; under sustained throttling it flaps.

Amplifier: the renewal's own `resolveByExactGet` is a state-2 GET, so under sustained throttling on
that key it can sit inside the SDK's retry loop for a long time (see Gap 8) before the controller
can even reach its deadline gate and publish the terminal verdict.

### Gap 8 — MEDIUM, cross-cutting. State 2 fails slowly: ~500 retries × 5 s cap ≈ 41 minutes {#gap-8-medium-cross-cutting-state-2-fails-slowly-500-retries}

`s3_retry_attempts` defaults to 500 and the backoff caps at 5 s with no jitter
(`src/IO/S3Defines.h`). A request that is throttled for its whole life therefore blocks its calling
thread for up to roughly 41 minutes before raising `S3_ERROR`. Every CAS read, HEAD, LIST,
`deleteExact` and blob publish inherits this. It is excellent availability and terrible latency: a
throttled read on the query path, on the GC fold, or inside a lease renewal's resolve GET does not
fail fast, it hangs. Nothing in the CAS layer bounds it, and the CAS operation deadline cannot
preempt a request already inside the SDK loop.

This is a configuration-shaped finding rather than a code one — a CAS-specific
`s3_retry_attempts` is the lever — but it belongs in this report because it interacts with Gap 7.

### Gap 9 — LOW. Post-commit generation reclaim is one-shot and leaks permanently {#gap-9-low-post-commit-generation-reclaim-is-one-shot-and-lea}

The hand-off reclaim in `Gc::runRegularRound` (`deletePrefixWholesale`, ~1003–1031) runs after the
round CAS. A `list` or `deleteExact` that throws after the SDK exhausts its retries strands that
generation's prefix permanently: `snap_pruned_through` is already past it and the parent-seal
difference that triggered the reclaim does not recur. The code documents this window for a crash;
a post-exhaustion throttling throw hits the same window. GC metadata only, never user data;
recoverable by `fsck`.

### Gap 10 — LOW. A controlled write's result is discarded on the blob-adopt path {#gap-10-low-a-controlled-writes-result-is-discarded-on-the-bl}

`PartWriteTxn::ensureBlobPresent`, adopt branch (`Pool/CasPartWriteTxn.cpp` ~366): the
`putMetaIfAbsent` backfill result is ignored, so `Unresolved` (the controller's budget exhausted
under throttling) and `Conflict` are both treated as success. The writer then proceeds with a blob
body that may carry no `Clean` marker. Per `Pool/CasBlobMeta.h` the marker is a hint and body safety
rests on the incarnation tag plus token-exact delete, so this is not corruption — but it is a
retryable error silently classified as success, which is the shape worth not having.

### Gap 11 — LOW. `DROP TABLE` latches a refusal before its single-attempt catalog write {#gap-11-low-drop-table-latches-a-refusal-before-its-single-at}

`CasRefLedger::dropNamespaceImpl` sets `removal_admission_closed = true` **before**
`beginRemoving`'s single-attempt catalog CAS. If that CAS throws 429 and the resolution read in the
catch also fails, the flag stays latched while the catalog row is still `Live`. The `DROP` fails with
`S3_ERROR`, and every later append on that table is refused with a message saying the namespace is
`Removing` — until an operator retries the `DROP` or remounts. Fail-closed and no data risk, but a
throttling error converted into a sticky state with a wrong explanation.

### Gap 12 — LOW. A failed occupant read faults a lane with a corruption code {#gap-12-low-a-failed-occupant-read-faults-a-lane-with-a-corru}

`CasRefLedger::commitRefChunk`'s `CORRUPTED_DATA` arm: if the occupant `backend.get` fails with a
post-exhaustion `S3_ERROR`, the lane is set `Faulted` ("until remount recovery adjudicates") rather
than deferred. Reachable only after the controller already proved a *different* object at the key,
so the writer is deposed either way; the difference is the terminal flavour and the error class.

### Also worth fixing, not a retry gap {#also-worth-fixing-not-a-retry-gap}

- `Backend/CasBackend.h`, the `ProbeOutcome` doc comment claims `head`/`get` flatten permission and
  transport faults into not-found. False for the S3 backend — they throw. The comment understates the
  subsystem's actual safety, and the GC and mount safety arguments depend on the narrower real set.
- `CasRequestBudget::recovery_retry_budget_ms`'s comment says a recovery attempt "(LIST + snapshot/log
  GETs + seal PUT)" is retried on "a transient `NETWORK_ERROR`". `isTransientRecoveryError` actually
  accepts six codes (`NETWORK_ERROR`, `S3_ERROR`, `POCO_EXCEPTION`, `SOCKET_TIMEOUT`,
  `CANNOT_READ_FROM_SOCKET`, `TIMEOUT_EXCEEDED`), and the walk performs no LIST at all.
- `PoolMeta::createOrValidate` and `admitOrValidate` raise `LOGICAL_ERROR` when the object is absent
  after a conflict; that is an external delete race, not a local bug.
- `allocateWriterEpoch` raises `CORRUPTED_DATA` for a non-`KeyAbsent` sentinel probe, including a
  transport failure. Fail-closed and the message is accurate, but the code is misleading.

---

## 4. The read side {#4-the-read-side}

Every read in the subsystem — `Backend::get`, `getStream`, `head`, `list`, and the query-path blob
and manifest fetches — goes through `IObjectStorage` on the **default** client, so all reads are
state 2 with ~500 SDK retries and a 429 among the retried statuses. **No read path lacks retry.**
Specifically checked and clear:

- `CasManifestReader::readManifestShared` and `ContentAddressedMetadataStorage::readBlobPayload`, the
  two reads on the `SELECT` path: state 2, and a post-exhaustion `S3Exception` is accepted by
  `checkDataPart::isRetryableException`, so a throttled manifest or blob read at part load does **not**
  detach the part as broken.
- The GC fold's discovery LISTs: `S3ObjectStorage`'s iterator throws on a failed `ListObjectsV2` and
  never returns a short page, so "listed nothing" cannot be produced by a throttling error and a
  reclaim decision cannot be reached on partial coverage.
- The ref-lane recovery walk: its transient filter accepts `S3_ERROR`, so a throttled GET inside
  recovery is retried for up to `recovery_retry_budget_ms` (120 s) rather than failing the table load
  outright.

The only read-side concern is latency, not correctness — Gap 8.

---

## 5. What I could not determine {#5-what-i-could-not-determine}

- **The 40 % figure itself.** I confirmed the mechanism from the code — two single-attempt CAS writes
  to one pool-wide key per `CREATE TABLE`, no retry, against a provider that rate-limits same-key
  mutations — and it is sufficient to produce the reported failure rate. The rate itself depends on the
  bucket's actual per-key ceiling and the concurrency of the run, neither of which is in the code.
- **Whether GC treats a marker-less blob body conservatively** (the consequence of Gap 10). The write
  side is clear; establishing the GC side would need a fold-path trace I did not run to conclusion.
- **The real distribution of consecutive throttled renewals.** The ~7-attempt fence-trip arithmetic in
  Gap 7 is derived from the default budget values in `CasRequestBudget` and the lease TTL; the actual
  count depends on per-attempt latency under throttling, which I could not measure without running
  against the live bucket.
- **Non-S3 Native backends.** Everything above is derived from `S3ObjectStorage`. The emulated and
  in-memory backends have different error surfaces, and a future non-S3 Native backend would need this
  audit redone: `checkConditionalWriteSingleAttemptSupport` gates the single-attempt requirement, but
  the 404-only flattening that carries the whole safety argument is an `S3ObjectStorage` property.
