---
description: 'Three read-only studies behind the mount lease settings notes: whether the second envelope of the write reserve is a safety matter, whether one control-plane attempt is bounded by a wall-clock deadline, and what control-plane requests take in practice.'
sidebar_label: 'Mount lease settings: studies'
sidebar_position: 105
slug: /superpowers/cas/mount-lease-settings-studies
title: 'CAS mount lease settings: three studies'
doc_type: 'guide'
---

# CAS mount lease settings: three studies {#mount-lease-settings-studies}

Evidence for [the settings notes](../specs/2026-10-02-cas-mount-lease-settings-ux-notes.md). Each study
read code or saved data; none built or ran anything. Code references are to commit `e1c38ca7478` of
branch `fix/antalya-26.6/cas-mount-renewal-no-deadline`.

## Q1: is the second envelope of the write reserve a safety matter? {#q1-q1-is-the-second-envelope-of-the-write-reserve-a-s}

Read-only audit at commit `e1c38ca7478`.
`CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`. Nothing was built, run or
model-checked.

### Verdict {#q1-verdict}

**It is not required for safety; it buys a settled outcome at the deadline edge, and 7 s of
incidental slack before the deadline for a conditional `PUT`.**

- The settling read never decides safety. With one envelope the engine returns
  `GaveUp{Deadline, Lease, sent_any = true, last_seen = NotObserved}`. The engine returns that same
  value today on three paths, one of them pinned by a test. Every consumer already treats it as
  "may have landed".
- What it buys: fewer wedged ref lanes and `NeedsRecovery` re-walks, and a `Conflict` instead of a
  give-up for a clean 412 at the edge.
- One residual that is NOT about the read: today every conditional `PUT` ends at least
  `envelope + margin` (9 s) before the deadline; with one envelope, `margin` (2 s). Whether those 7 s
  hide an attempt that overruns its envelope is open question 2 of the design note. Not established here.

**Failure asymmetry.** A wrong "not needed" loses data silently. A wrong "needed" costs 7 s of write
availability per outage. Keeping two envelopes is the safe direction. The verdict rests on reading,
not on a run, so the gate in the last section comes before any change.

### 1. Who reserves what {#q1-callers}

All names at `e1c38ca7478`. The lease enters only through the mount-plane fence
(`CasMountRuntime::admit`, `CA/Pool/CasMountRuntime.cpp:146`: `needed + margin < remaining`) and through
`Retry::untilLeaseSafe` bounds checked by `fits`.

| Site | Reserves | For what |
|---|---|---|
| `CasOperation::writeLoop`, `CA/Backend/CasRequests.cpp:927` | `reservedFor(0, 2)` via `gate` and `fits` | the `PUT` and the read that settles it |
| `pauseAndReissue` `:874`, `pauseForConflict` `:880`, `pauseFlat` `:889`, `reissueAtOnce` `:894` (all `gatedPause` `:846`) | `pause + 2` | the next `PUT` and its read |
| `CasOperation::readLoop`, `CA/Backend/CasRequests.h:493`, `:533`, `:550` | `pause + 1` | one read, head, list, **`remove`, `removeManyWriteOnce`, blob `publish`** |
| `CasOperation::removeCurrent` `:644` | `pause + 2` | the next `head` and the `remove` after it: two requests, not a settling read |
| `CasOperation::probeSentinel` `:664`, `:703` | `1`, `pause + 1` | one probe |
| `CasOperation::postCommit` `:828`; `admitted` | `0` | a verdict, no request |
| `CasMountRuntime::refAppendFenceOk`, `CasMountRuntime.cpp:162` | `2 × attemptEnvelopeMs` | not a write. Feeds `fence_ok_fn`: relink confirm (`CasRefLedger.cpp:552`), recovery retry loop (`:1619`, `:1661`), its sleep (`:241`) |
| `validateCasRequestBudget`, `CA/Backend/CasRequestBudget.cpp:30` | `envelope + margin < TTL`; `period + 2 × envelope + margin < TTL` | startup check |
| `MountPoint` open, `CA/Pool/CasPool.cpp:876`; remount, `:1553` | `period + 2 × envelope` before `anchor + TTL − margin` | else re-anchor with one synchronous renewal before arming |
| `MountLeaseRenewer::terminate`, `CA/Pool/CasServerRoot.cpp:1766` | window `max(10 s, 2 × reservation + 2 s)`, under `untilLeaseSafe` | so the farewell's own `writeLoop` reservation fits |
| `drainRefLanesForShutdown` budget, `CasPool.cpp:1048`, `:1203`; detached drain, `ContentAddressedMetadataStorage.cpp:1018` | `envelope + margin` | one attempt; already one envelope |

`budgetAdmits` and `refAppendReservationMs` do not exist at `e1c38ca7478`. They are on
`refactor/antalya-26.6/cas-mount-one-lease-thread` (`e5e1dcbf283`, `CasMountRuntime.cpp:154`, `:166`),
where `canArm` (`:349`) is the readiness rule "the deadline admits a ref append".

Planes (`CasPool.cpp:189`-`:216`): only `mount_requests` carries the lease fence. `gc_requests` refuses
only at teardown. `farewell_requests` and `lease_requests` are `Fence::open`.

No observer-side code uses an envelope. `mountObservationThresholdMs` (`CasServerRoot.cpp:939`) is
`TTL + TTL/20 + cadence`. An untruncated grep for `envelope` outside tests finds only holder-side
admission, validation, drain and farewell windows.

### 2. The unclear `PUT` with less than one envelope left {#q1-walk}

**Engine today.** The `PUT` was admitted with two envelopes, so the read starts. Then:

- read sees our bytes with the precondition moved → `postCommit` → `Committed`;
- read sees the precondition still satisfiable, or fails → reissue needs `pause + 2` → refused →
  `GaveUp{Deadline, Lease, sent_any}`.

**Engine with one envelope.** `observe` → `readLoop` gate (`reservedFor(0, 1)`) → `NoBudget` →
`giveUpReadNoBudget` sets `ReadStop::NoBudgetLease` → `writeLoop` `:1083`
`gaveUpForReadStop` → `GaveUp{Why::Deadline, Source::Lease, sent_any = true, last_seen = NotObserved}`.
Never `Unresolved`, never `Conflict{NotObserved}`.

**The same value is reachable today**, with the second envelope reserved:

1. `PUT` unclear, the read's first attempt fails, its reissue is refused: same `NoBudgetLease` stop.
2. Connect-failure hint: `pauseFlat` refuses, give-up with `sent_any` and **no read at all**
   (`writeLoop` `:1056`). Pinned by `CASRequestsConnectHint.GatesRefuseTheReissue`, third block
   (`src/Disks/tests/gtest_cas_requests.cpp:2835`, "the documented deadline-edge difference").
3. `PUT` returns 2xx and `postCommit`'s `gate(0)` refuses: give-up for a write that landed.

The lease edge is reached only when renewals have failed for 14 s, so the store is usually failing
for the read as well. Path 1 is the common case of an unclear `PUT` at the edge today.

**Per caller** (all ten non-test consumers of `GaveUp`, untruncated grep):

| Caller | On `GaveUp` with `sent_any` |
|---|---|
| Ref-log append, `commitRefChunk` (`CA/Pool/CasRefLedger.cpp:3731`, arm at `:3906`) | lane `Writing → Wedged`, the exact key and bytes stay installed, every queued caller gets retry-later. Without `sent_any`: back to `Ready`, id not consumed |
| Wedge retry, `resolveWedgeOnce` (`:2348`) | stays `Wedged` (`unresolved`) |
| Epoch seal in recovery (`:1154`, `:1203`) | throw retry-later; table stays unrecovered; next walk re-derives the slot |
| `_ckpt`, `publishCkptContribution` (`CA/Pool/CasRefCkpt.cpp:270`) | `FenceLost` → `FencedOut`; else throw → `requireRecovery` in the ledger |
| Ref snapshot (`CasRefLedger.cpp:4544`) | no prune, backoff |
| Manifest body, `stageManifest` (`CA/Pool/CasPartWriteTxn.cpp:679`) | throw; the id is never named; caller re-stages under a fresh `ManifestId`; a late body is debris |
| Blob `.meta` to `Clean` (`CasPartWriteTxn.cpp:354`-`:370`), plain objects (`CasPlainObjects.cpp:19`) | `orThrow` → retry-later; the build or statement fails |
| Ref catalog (`CasRefCatalog.cpp:198`, `:516`) | throw; the erase path re-reads the whole catalog before any conclusion |
| Hot-key lane (`CA/Backend/CasHotKeys.cpp:284`) | forgets the cached entry |
| `gc/state` (`CA/Gc/CasGc.cpp:1149`, `:4734`), janitor cursor | GC plane, no lease: unaffected at the lease edge. At its 90 s policy edge: `orThrow`, next round re-reads |
| Mount renewal, `renewOn` (`CasServerRoot.cpp:1592`) | background: `worker_requests` (`Fence::open`) + `Retry::untilDefinitive`: `gate` and `fits` never refuse on time. **Not affected: confirmed.** Startup, remount, direct: `LeaseBound`, cutoff moves, give-up → `RenewalTerminal` as today |
| Farewell, `terminate` (`:1790`) | `orThrow`; no farewell; successor observes the token for the full threshold |

**The wedged lane.** `Wedged` holds one `RefAppendAttempt` (key, bytes, admitted generation). Meanwhile
refused: every append of that namespace (`flushRefBatch` resolves the wedge first, `:2826`), relink
confirm (`:515`, answer `Unknown`), a clean farewell (`drainRefLanesForShutdown`, `:2032`). Resolved by:

- the next mutation of that namespace: one conditional create of the same key and bytes under the
  wedge's own generation, then a post-I/O `op.admitted` recheck. Created or byte-equal occupant →
  frontier, adopt. Successor seal → `Closed`. Foreign → `Faulted` and anomaly;
- or a remount: `quiesceRefTablesForRemount` drops the runtime and the new epoch's `EpochSeal`
  conditional create settles it (`:1847` comment, `:1154`).

No background resolver exists (`CasRefLedger.h:1169`).

### 3. Safety checks {#q1-safety}

**(a) Acknowledge a write that did not land. No. PROVEN** (engine and ledger, end to end).
`Committed` is built only in `postCommit`, reached from a 2xx with a valid etag or from a read that
shows our bytes under a moved precondition (`writeLoop`). A give-up never becomes an acknowledgement:
`commitRefChunk` completes survivors with an error, `resolveWedgeOnce` acknowledges only on `Created`
or byte equality. One envelope removes a read; it adds no path to `Committed`.

**(b) Retry with different bytes that commits twice or over a landed write. No. PROVEN for the ref
log; BELIEVED for other keys.** The wedge pins `(writer_epoch, sequence)` and its bytes; the id returns
to the allocator only on `Refused` without ambiguity or on `!sent_any` (`:3929`), both of which sent
nothing that could apply. Manifest ids are never reused (`next_manifest_ordinal`). For `_ckpt`, catalog,
`.meta`, plain objects the argument is the store's conditional write (one of two writers on one token
wins); not followed above the CAS directory.

**(c) Treated as not landed, then blobs or refs released. No. PROVEN in the ledger; BELIEVED above it.**
No consumer maps `sent_any` to "did not land". An uncertain precommit keeps its build sequence active
and queues a cleanup duty (`PartWriteTxn::~PartWriteTxn`, `CasPartWriteTxn.cpp:152`); the duty runs
through the same lane, after the wedge resolves. `writerCleanupDutiesPending` blocks the clean farewell
(`CasPool.cpp:1049`). GC eligibility has no clock term (lapse audit, section 4; not re-read here).

**(d) A later reader settles the outcome without authority. No. PROVEN for the engine and the wedge;
BELIEVED for recovery.**
- Every mount-plane read is gated: `readLoop` needs `envelope + margin` of lease. No read starts in the
  last 9 s, with one envelope or two.
- A result is acted on only after a fresh admission: `postCommit` `gate(0)`; `resolveWedgeOnce`
  `fence_moved = !op.admitted()` under `state_mutex`.
- Settlement after the lapse is a conditional create, not a read (`CasRefLedger.h:1177`: "A read
  alone could only ever report absent, which is not a rejection").
- Under a new epoch the seal is a conditional create that adopts a straggler and retries at the next
  slot (`CasRefLedger.cpp:1181`-`:1198`). The walk's `LIST` trust was not re-audited.

**(e) A request sent after the deadline. No. PROVEN, given the envelope bounds one attempt.**
With one envelope a `PUT` starts only while `remaining > envelope + margin`: the rule every read,
`remove`, batch delete and blob `publish` follows today. Latest start of any request stays
`deadline − margin − envelope`. What moves is the latest END of a conditional `PUT`:
`deadline − margin − envelope` → `deadline − margin`.

**Model.** `CaCasMountCore.tla`: `Write` is atomic at `clock < fenceUntil`; `WedgeAdmit` and
`StragglerLands` carry no clock guard, and no action is a settling read. `AckedOpsAreDurable` and the
superseded-writer invariants are stated over ambiguous attempts that are never settled by a read and
may land at any time until a seal. The model has no request duration, so it speaks to neither envelope
as a timing device. `CaMountRenewRetryCore.tla` covers the renewal only. Results taken from
`CaCasMountCore_RESULTS.md`, not re-run.

### 4. Cost and frequency {#q1-cost}

Let `R` be the lease left when a write is admitted. Defaults: envelope 7 s, margin 2 s.

| `R` | Today | One envelope |
|---|---|---|
| > 16 s | sent; a read fits after an unclear `PUT` | same |
| 9 s to 16 s | refused, nothing sent, lane stays `Ready`, id not consumed | sent. Clear answer: commit or refusal. Unclear after `t`: read admitted only if `R − t > 9 s`, else give-up with `sent_any` |
| ≤ 9 s | refused | refused |

- Gain: writes stop 21 s after the last renewal instead of 14 s; tolerated outage 11 s instead of 4 s.
- New unresolved outcomes arise only from a `PUT` started in the 9-16 s band that goes unclear late.
  That band is entered after renewals have failed for 14 s, so such a `PUT` is likely to be unclear.
  In a full outage the band adds wedges where today there are clean refusals.
- A lane with a writer active during an outage that reaches the cutoff already wedges today
  (connect failures set `sent_any`; lapse audit B4). The addition is namespaces whose first write of
  the outage falls in the band.
- A clean 412 in the band becomes a give-up, hence a wedge, instead of `Conflict`. For a ref-log create
  a 412 means a successor seal or interference: rare, and the next flush adjudicates it.

Cost of one unresolved append:

| Item | Cost |
|---|---|
| Statement | INSERT, merge commit or part removal fails with retry-later |
| Lane | wedged until the next mutation of that namespace or a remount |
| Relink confirm on that table | `Unknown` |
| Shutdown | no farewell; next start waits 36.5 s |
| Watermark | cleanup duty pins `min_active_build_sequence` until the namespace is next mutated |
| Memory | one encoded transaction per wedged lane |
| Other outcomes | `_ckpt` unresolved after a durable append → `NeedsRecovery`, one table re-walk |

### 5. What changes with a one-envelope reserve {#q1-changes}

- `writeLoop` reservation and the four `gatedPause` callers: 2 → 1. `removeCurrent` stays 2.
- `refAppendFenceOk` (`refAppendReservationMs` and `canArm` on the other branch) must equal the
  engine's reservation, or `fence_ok_fn` refuses what the engine admits. The relink confirm then answers
  `Yes` down to 9 s of lease; decide whether that answer wants its own window (BELIEVED harmless: the
  answer is instantaneous; relink protocol not read).
- `validateCasRequestBudget`: cadence check becomes `period + envelope + margin < TTL`. Its stated
  reason ("a renewal is a write") no longer binds the background renewal; what it guarantees is that
  writes are admissible when a renewal is due.
- `CasPool.cpp:876`-`:905` and `:1553`-`:1577`: `renewal_window_ms`.
- `terminate`: the farewell window may stay; its comment cites `reservedFor(0, 2)`.
- Text: `CasRequests.cpp:925`, `CasRequestBudget.h:9`-`:55`, `CasPool.h:927`,
  `ContentAddressedSettings.cpp:75` (`unsafe_remount_no_delay`: "confirmed deadline − margin −
  2 × envelope"), `:85`; `docs/en/antalya/cas/architecture/mounts-and-leases.md:103`, `:167`, `:170`;
  `docs/en/antalya/cas/configuration.md:100`, `:111`, `:112`, `:130`.
- Tests that pin the cutoff, all under `src/Disks/tests`:
  `gtest_cas_requests.cpp:2224` (`AWriteReservesTwoEnvelopesSoOneOfSurplusStartsNothing`), `:2835`,
  `:3214`-`:3239` (`largest − 14'000`), `:3547`, `:3582`; `gtest_cas_mount_runtime.cpp:173`, `:190`;
  `gtest_cas_pool.cpp:2758`; `gtest_cas_heartbeat.cpp:389`; `gtest_cas_hot_keys.cpp:735`;
  `gtest_cas_ref_contiguous_alloc.cpp:99`; `gtest_cas_ref_install_safety.cpp:104`.

### 6. What must be shown before the reserve is reduced {#q1-gate}

Predictions, none run (read-only dispatch):

1. **Value equality.** Injected clock, reservation 1000, lease-bound policy. Case A (today's code):
   `PUT` throws a timeout, the read throws, no room to reissue. Case B (one envelope): `PUT` throws a
   timeout with less than one reservation left. Both return
   `GaveUp{Deadline, Lease, sent_any, NotObserved}`; B issues zero reads. If B returns anything else,
   section 2 is wrong.
2. **Ledger.** That result leaves the lane `Wedged` with the same key and bytes. After a renewal under
   the same generation the next flush adopts a landed straggler by byte equality or lands the create.
   After a remount the seal either follows the straggler or defeats it.
3. **Model.** Add request duration to a request-level model: a `PUT` in flight over an interval, the
   observer free to act at `TTL + TTL/20 + cadence`. Show no `PUT` starts inside `envelope + margin`
   of the deadline and the existing invariants hold with the straggler landing in the last 2 s and
   after it. Include one non-ledger conditional key (`_ckpt` or blob `.meta`); today's model has only
   the ref-log slot.
4. **Question 2 first.** Show one attempt cannot outlive its envelope (a whole-request deadline, or a
   proof from `PocoHTTPClient`). The request timeout is set as a socket timeout (`setTimeouts`,
   `src/IO/S3/PocoHTTPClient.cpp:683`-`:706`); a slow send then a slow receive may each take up to the
   timeout. Today an overrun of up to 9 s still ends before the deadline; with one envelope, 2 s.
   `remove` and `publish` already run with 2 s.
5. **Soak.** Outages of 4 s to 11 s and a `PUT`-only brown-out: `CASRefAppendWedged`,
   `CASRefAppendPreAttemptRefused`, fsck clean.

### Limits {#q1-limits}

- By reading only. "Same value" is two code paths compared by eye plus one existing test I did not run.
- Late landing of non-ledger conditional writes was not audited. It is unchanged in kind: the same
  give-up exists today. Its window before the deadline shrinks from 9 s to 2 s.
- Callers above the CAS directory (MergeTree reaction to retry-later) were not followed.
- Recovery's walk and `LIST` trust were taken as settled.
- Store conditional-write semantics are assumed.
- The claim covers the settling read. It does not cover the margin, the first envelope or the
  observer thresholds.


## Q2: is the 7 s attempt envelope a wall-clock bound? {#q2-q2-is-the-7-s-attempt-envelope-a-wall-clock-bound}

Read-only study of commit `e1c38ca7478`. Nothing was built or run. "Inferred" marks what the code implies but I did not observe.
T = `attempt_timeout_ms` = 5000, C = connect cap = 1000, envelope = T + 2C = 7000 ms.

### Answer {#q2-answer}

1. No. Nothing bounds a whole attempt: `connect`, `send` and `receive` timeouts each bound ONE socket operation, and every `recv`/`send` call restarts the full timeout (`SocketImpl::receiveBytes:333`, `sendBytes:286`). No `httpRequestTimeoutMs`, stopwatch deadline, cancellation check or watchdog exists in `PocoHTTPClient`, the AWS SDK path, `S3::Client`, the CAS backend or `CasRequests`.
2. A single complete stall in any one phase ends within 7.0 s, so the envelope is exact for that case. It is exceeded without limit by a trickling server (one byte per 4 s), by sequential near-timeout phases (12 s for a `GET` with a stalled body, 17 s or more for a `PUT` with `Expect: 100-continue`), and by transparent extra requests inside one attempt (`307` redirects, SDK region re-sign, `HeadBucket` region probe, reconnect on first write).
3. Smallest change: one absolute deadline per attempt, set by the backend in a thread-local scope (`now + envelope`), read by `PocoHTTPClient` at each request start and by `Poco::Net::HTTPSession` (`connect`/`send`/`receive` clamp their timeout to the time left and throw `TimeoutException` once it has passed). The session lives inside the response, so the clamp also covers the body read after `makeRequestInternalImpl` returns.

### How the CAS clone is configured {#q2-how-the-cas-clone-is-configured}

- `S3ObjectStorage::getSingleAttemptClient` (`S3ObjectStorage.cpp:1212`): clone of the disk client via `cloneWithConfigurationOverride` (:1247). Sets `requestTimeoutMs = T` (:1238), caps `connectTimeoutMs` at C (:1244), `max_retries = 0` and `SingleAttemptRetryStrategy` (:1228).
- Inherited by config copy (not overridden): `s3_use_adaptive_timeouts` (disk setting, default true: `S3Defines.h:18`, `diskSettings.cpp:181`), `s3_max_redirects` (default 10: `S3Defines.h:36`), `request_throttler`, proxy resolver, `http_client` (GCS dialects), region and `detect_region` inputs.
- `PocoHTTPClient` turns the config into `ConnectionTimeouts` (`PocoHTTPClient.cpp:210`): connect = `connectTimeoutMs`, send = receive = `requestTimeoutMs`. `http_receive_timeout` and the other `Settings` HTTP timeouts are not consulted on this path.
- Writes always take this client. Reads and heads take it only for a writable Native mount (`single_attempt_control_plane`, `ContentAddressedMetadataStorage.cpp:820`); a read-only mount uses the disk client (30 s timeouts, SDK retries). `ObjectStorageBackend::stream` also uses ordinary read settings by design (`CasObjectStorageBackend.cpp:683` comment).
- The engine checks the deadline BEFORE issuing an attempt: `CasOperation::fits` (`CasRequests.cpp:446`) with `attempt_reservation_ms = backend->attemptEnvelopeMs()` (:289). It never interrupts an attempt in flight.

### Phase table {#q2-phase-table}

Values are for attempt >= 2 or adaptive timeouts off (the widest case). Attempt 1 is tighter, see point 5.

| Phase | What bounds it | Value (CAS control plane) | Evidence |
|---|---|---|---|
| Client-side throttler (`s3_max_get_rps`/`s3_max_put_rps`, default 0 = off) | `Throttler::throttle(1)` with `max_block_ns` = unlimited; sleeps `deficit / rps`. Bucket is shared with the disk client (config copy), so data-plane traffic delays control requests | unbounded when enabled; no timer | `PocoHTTPClient.cpp:makeRequestInternalImpl:510,517`; `HTTPRequestThrottler.cpp:17`; `Throttler.cpp:94`; `Poco/Net/Throttler.h` (`unlimited_block_ns`) |
| Proxy resolution | Remote resolver has its own 1 s connect/send/receive, cached by TTL | <= ~3 s on a cache miss (inferred from three 1 s timeouts) | `RemoteProxyConfigurationResolver.cpp:79` |
| DNS | `HostResolver::resolve` re-resolves every 40 s (history 120 s / 3) and after `setFail`; goes through `DNSResolver` cache, a miss calls blocking `getaddrinfo` with no ClickHouse timeout. IP-literal endpoints skip it. Runs only when a NEW connection is made | ~0 when cached; cold miss bounded only by libc resolver config | `HTTPConnectionPool.cpp:prepareNewConnection:846`; `HostResolvePool.cpp:137`; `DNSResolver.cpp:507` (FIXME comment) |
| Waiting for a pooled connection | No wait. Pool pops a stored connection or connects a new one. Hard limit throws `HTTP_CONNECTION_LIMIT_REACHED` (200000 default). `maxConnections` from the client config is not enforced here | 0 s | `HTTPConnectionPool.cpp:getConnection:698`, `atConnectionCreate:242` |
| TCP connect | Non-blocking `connect` + `poll(connection_timeout)` | C = 1000 ms | `SocketImpl.cpp:connect:122`; `HTTPSession::connect` |
| TLS handshake | Eager (nobody sets lazy handshake). `SecureSocketImpl::connect` copies the connect timeout onto the socket (:168) and runs `SSL_connect` with a cumulative `remaining_time` counter (:226) | <= C = 1000 ms, so TCP + TLS <= 2C; confirms the code comment | `SecureSocketImpl.cpp:168-170,226`; `SecureStreamSocketImpl.cpp:99` |
| Send request headers | Per `send` call: `poll(POLLOUT, send_timeout)`. Headers fit the kernel buffer, so a stall cannot occur here unless the peer's window is closed | T per call | `SocketImpl.cpp:sendBytes:280` |
| `Expect: 100-continue` wait (only `PUT` with `If-Match`/`If-None-Match`/`x-goog-if-generation-match` and body >= `expect_continue_min_bytes`; the CAS clone raises 0 to 1 MiB) | `peekResponse` reads under the receive timeout set with `first_byte = true`; no separate timer. `receiveResponse` loops while status is `100` | T per recv; the loop is unbounded | `S3ObjectStorage.cpp:getSingleAttemptClient` (fallback 1 MiB); `PocoHTTPClient.cpp:684`; `HTTPClientSession.cpp:372` |
| Send body | `setTimeouts(..., first_byte=false)` then `StreamCopier::copyStream`; send timeout per `send` call | T per call; total = size / drain rate, unbounded | `PocoHTTPClient.cpp:700-701` |
| Wait for response headers | Receive timeout per `recv` (the `setTimeouts` at :706 runs before `receiveResponse`, so the 200 ms "first byte" value applies only to connect and header send). Poco header limits: 1e6 fields, 128 KiB each | T per recv; a trickle is bounded only by those limits | `PocoHTTPClient.cpp:706,710`; `PocoHTTPClient.h:100-101` |
| Read response body | Same per-`recv` timeout, but AFTER `makeRequestInternalImpl` returned: the session rides in the response (`SetResponseBody(stream, session)`, :820) and `ReadBufferFromS3::nextImpl` reads it. Reads stop at `casMaxStoredObjectBytes` (about 256 MiB: `ZSTD_compressBound` of the largest `object_cap`) | T per recv; total unbounded | `PocoHTTPClient.cpp:820`; `S3ObjectStorage.cpp:readSmallObjectAndGetObjectMetadata:377`; `CasFormat.cpp:139` |
| Reuse of a stale pooled connection | Failure on the first write: `HTTPClientSession::write` sees `_reconnect` (set at :295 before the write), closes, reconnects (`PooledConnection::reconnect` pops another pooled connection or connects new) and rewrites, all inside the same attempt. Applies to fresh connections too. A failure at read time is not retried | +<= 2C (new TCP + TLS) once, so connect cost can be paid twice: up to 4C = 4 s before I/O | `HTTPClientSession.cpp:295,454`; `HTTPConnectionPool.cpp:432` |
| `307` redirect | New session per iteration with fresh timeouts; the `Stopwatch` in the loop only feeds metrics | <= `s3_max_redirects` + 1 = 11 requests (77 s at 7 s each) | `PocoHTTPClient.cpp:557,743` |
| Region re-sign by the SDK | `retryWithCorrectRegion` skips `ShouldRetry` for `301/307/400/403` naming another region when the client region is `AWS_GLOBAL` (region unset). The code already notes it (`S3ObjectStorage.cpp:1087`) | one extra full request per distinct region named (normally 1) | `contrib/aws/.../AWSClient.cpp:330-357` |
| `S3::Client::doRequest` outer loop | Repeats `request_fn` on `AuthenticationRequired`, wrong-region `400`, `301`, `IllegalLocationConstraint` | <= 11 full requests | `Client.cpp:730` |
| Region probe | AWS provider with unset region (`detect_region`): `getRegionForBucket` issues `HeadBucket` on the same client while holding the region-cache mutex; when no region is learned it repeats on every request | +1 request (<= envelope) on every request, plus mutex convoy | `Client.cpp:334,1049-1065` |
| Credentials at signing | Provider `GetAWSCredentials` runs in `AttemptOneRequest` on the attempt thread. Static keys: none. IMDS: own client (connect 50 ms, request 1000 ms, SDK `DefaultRetryStrategy(1, 1000)`). STS/web identity/SSO: config clone, up to the disk's 30 s | not bounded by the envelope; once per credential lifetime | `Credentials.cpp:1116-1119` |
| Credentials refresh callback inside the object storage | `refreshAndRetryOnExpiredCredentials` reruns the call after a refresh (HEAD, conditional `DELETE`, batch `DELETE`). The disk's callback returns `nullptr`, so it rethrows | no rerun for disks | `S3ObjectStorage.cpp:280-294,608,664,890`; `S3ObjectStorage.h:46` |
| SDK, `S3::Client` and write-buffer retries | `max_attempts = 1`; `ShouldRetry` is false; write buffer `max_unexpected_write_error_retries` forced to 1; read buffer `max_single_read_retries = 1` | no transparent retry | `Client.cpp:196,811`; `S3ObjectStorage.cpp:336`; `WriteBufferFromS3.cpp:677,778` |

### Point 5: adaptive timeouts and per-method overrides {#q2-point-5-adaptive-timeouts-and-per-method-overrides}

- `PocoHTTPClient::getTimeouts` (:416) calls `ConnectionTimeouts::getAdaptiveTimeouts` when `s3_use_adaptive_timeouts` is true (default). The clone inherits the flag.
- It applies only when `first_attempt = ch_attempt == 1 && sdk_attempt == 1` (:488). CAS seeds `object_storage_attempt_number` with its own 1-based attempt count (`CasObjectStorageBackend.cpp:612,832`; `Requests.cpp:227` defaults to 1 when absent), so attempt 1 is adaptive and every reissue (attempt >= 2) gets the full `T` and `C`.
- `saturate(x, limit)` returns `min(x, limit)` (`ConnectionTimeouts.cpp:33`): adaptive values only LOWER the configured ones, never raise them. For attempt 1: connect 500 ms; header send 200 ms; body send `PUT` 1000 ms; response wait `PUT` 3000, `GET`/`HEAD` 500, `DELETE` 5000 (10000 capped), `POST` 5000 (`ConnectionTimeouts.cpp:100-125`). Effect: the first attempt is tighter than the envelope and can time out spuriously on a healthy store; the envelope is only reached from attempt 2.
- No per-method override of the 5 s exists besides this table.

### Point 6: GCS {#q2-point-6-gcs}

- Same code path, not a native client. GCS runs through `S3::Client` with `http_client = gcs_hmac` (`PocoHTTPClientGCSHMAC`) or `gcp_oauth` (`PocoHTTPClientGCPOAuth`). Both call `PocoHTTPClient::makeRequestInternal` after translating the request to the `x-goog-if-generation-match` dialect (`PocoHTTPClient.cpp:1006-1026`, `Client.cpp:1000`). All rows above apply.
- Differences:
  - `gcs_hmac` signs locally with the shared provider: no network at signing.
  - `gcp_oauth` fetches the bearer token inside `makeRequestInternal` under the client mutex (`PocoHTTPClient.cpp:918-920`) over `timeouts` (T per socket operation, no deadline). Every clone builds its own `PocoHTTPClientGCPOAuth` (inferred from the SDK client constructor), so the first request on a new single-attempt clone pays a token fetch, and concurrent requests queue behind it.
  - Generation dialect forces one `PUT` (`s3_force_single_part_upload`); no `HeadBucket` probe (`detect_region` needs the AWS provider); the region re-sign path needs `x-amz-bucket-region`, which GCS does not send (inferred).
  - `Expect: 100-continue` is negotiated for `x-goog-if-generation-match` as well (`PocoHTTPClient.cpp` condition above the peek).

### Point 4: worst-case wall time of one attempt (TLS, warm DNS, attempt >= 2) {#q2-point-4-worst-case-wall-time-of-one-attempt-tls-wa}

(a) Server stalls completely at one phase:

| Stalled phase | Time to error |
|---|---|
| DNS cold miss | not bounded by ClickHouse |
| TCP connect (SYN dropped) | 1.0 s |
| TLS handshake | 1.0 s after TCP, so <= 2.0 s |
| Response first byte (small `PUT`, `HEAD`, `GET`) | 2.0 + 5.0 = 7.0 s (= envelope) |
| 100-continue (body >= 1 MiB) | 7.0 s |
| Body stalled after headers arrived just under T | 2.0 + 5.0 + 5.0 = 12.0 s (`GET`) |
| `Expect` `PUT`: 100 just under T, body send stall, response wait | 2.0 + 5.0 + 5.0 + 5.0 = 17.0 s; more for bodies over several `send` calls |
| Throttler sleep | `deficit / rps`, no limit |
| Pool at its limit | 0 s (throws) |

(b) Trickle of one byte per 4 s: unbounded. Headers: the status line and headers alone are ~200-400 bytes, about 15-30 min, and Poco allows up to 1e6 fields of 128 KiB. `GET` body: up to `casMaxStoredObjectBytes` bytes at 4 s each (1 MiB is about 48 days). The 5 s timeout is never hit because every `recv` returns data.

(c) Throttler: unbounded while `s3_max_*_rps` is set. Pool limit: no wait.

Multipliers on top of any row: up to 11 requests for `307` or the outer `doRequest` loop; 2 requests for region re-sign or `HeadBucket` probe; +2 s for connect repeated on first-write failure.

### Open ends {#q2-open-ends}

- Whether the TLS handshake stays <= C under a trickling peer: `SSL_connect` blocks inside OpenSSL with `SO_RCVTIMEO = C` per read and the cumulative counter is consulted only when it returns (inferred bound: handshake bytes x C).
- Cold DNS time: depends on `resolv.conf` (timeout, attempts, search list) and on whether `DNSResolver` has the host cached; not observable from code.
- Which credentials provider a production CAS disk uses; the signing-time cost depends on it.
- Effective `s3_max_redirects` for the CAS disk (default 10 assumed; the server setting and `max_redirects` request setting can change it, `diskSettings.cpp:124-130`).
- Whether any real store returns `307` or region-naming `400/403` for the CAS endpoints. The code paths exist; I did not check which stores trigger them.
- A client-side deadline abandons the socket but cannot recall bytes already sent: a conditional `PUT` may still land after the deadline. That is the existing "ambiguous attempt" class, resolved by the settlement read, not something the deadline changes.
- Throttler and resource-scheduler hooks (`ResourceGuardSessionDataHooks::atStart`, `HTTPConnectionPool.cpp:~357`) can also block without a bound when a workload scheduler is attached to the thread; I did not check whether CAS threads ever carry one.


## Q3: measured latency of CAS control-plane requests (3 s / 0.5 s timeout check) {#q3-q3-measured-latency-of-cas-control-plane-requests}

### Answer {#q3-answer}

Not decidable from the data that could be read. No source gives per-request control-plane latency with p99/p99.9.

What the data does show:

- Typical requests are far below 3 s: means of 8-10 ms (MinIO, CI), 25 ms GET (AWS S3, otel.demo), 50-100 ms GET and 150-400 ms write (GCS over WAN).
- Bad moments exceed 3 s and 5 s equally. Every observed stall was a 5-12 s event (CI stall, GCS DNS outage, GCS connect-timeout storm), so 3 s would not have survived a stall that 5 s did not, and neither survives them.
- Share that would have timed out: unknown. Best bounds are the share of conditional writes ending "unresolved" (timeout, conflict, connection loss, 5xx together): 0.05% on AWS S3 over a week, 0.30% in CI on MinIO. Those ran with the old timeout, so they bound the 5 s case, not the 3 s case.
- Nothing measured a 0.5 s connect. GCS mean connect was 40-85 ms, so the mean is far below 0.5 s. Its tail is unmeasured, and a ten-minute connect-timeout storm to GCS was seen.

### Not reachable {#q3-not-reachable}

- Grafana at `localhost:3000`: not up (`curl -m 5 localhost:3000/api/health` -> HTTP 000, rc 7). Not started, per instructions.
- `localhost:19900` is listening (likely the otel.demo ClickHouse native port-forward). It was outside the allowed sources, so I did not query it. This is the one place where live `system.histogram_metrics` (`S3FirstByte`, `DiskS3FirstByte`) and `system.opentelemetry_span_log` could be read.
- `docker ps`: only `buildx_buildkit_clickhouse-regression-builder`. No soak stand is running.
- Soak server logs `utils/ca-soak/logs/**/clickhouse-server*.log`: owned by `syslog`, mode 0640, 182 of 272 files unreadable (I did not change permissions). The `_archive_*.tgz` files are 113 bytes (empty).
- `system.cas_log` dumps (`utils/ca-soak/logs/predown/ch{1,2}/*/cas_log.tsv`, 100 files, 997k rows) were read. They hold no `watermark_renew`, no `elapsed_ms`, no `attempts_sent` (event types are `root_add`, `ref_resolve`, `blob_put`, ...). Unusable for latency.
- No saved histogram exports, `blob_storage_log` with durations, or span dumps exist in `tmp/` or `utils/ca-soak/`.

### Source 1: CI job metric_log, MinIO, ASan (the only per-second series) {#q3-source-1-ci-job-metric-log-minio-asan-the-only-per}

- Store: MinIO on `localhost:11121`, bucket `test`, prefix `cas_s3` (CAS S3 storage policy). Job: "Stateless tests (amd_asan_ubsan, cas s3 storage, parallel, 1/2)", PR branch `backport/antalya-26.6/115832`, sha `9091be4a672`.
- File: `tmp/cas223/ci0907/metric_log.tsv.zst` (16,538 rows, 1 s each, 2026-09-07 14:00:17 to 18:35:52 UTC) and `tmp/cas223/ci0907/clickhouse-server.log.zst`.
- Cannot separate control plane from data: `S3WriteMicroseconds` / `S3ReadMicroseconds` cover all PUT/POST and all GET/HEAD/LIST of the ordinary S3 client (blobs, manifests, ref logs, tests' own S3 use). Per-second means only; no per-request values. ASan slows the server, so tails are pessimistic.

Totals over the run:

| | requests | mean | errors | throttling | retryable |
|---|---|---|---|---|---|
| S3 write (PUT/POST) | 1,571,436 | 9.9 ms | 3,753 | 0 | 0 |
| S3 read (GET/HEAD/LIST) | 4,809,173 | 3.5 ms | 245,725 (404 etc.) | 0 | 0 |

Per-second mean latency (seconds with at least one request; `n<=2` rows approximate single requests):

| series | seconds | p50 | p90 | p99 | p99.9 | max | secs >1 s | >3 s | >5 s |
|---|---|---|---|---|---|---|---|---|---|
| write, all | 16,523 | 7.9 ms | 15.9 | 56.2 | 147.6 | 10,673 ms | 2 | 1 | 1 |
| write, n<=2 | 33 | 7.8 | 140.5 | 7,500 | 10,355 | 10,673 | 1 | 1 | 1 |
| read, all | 16,533 | 2.3 ms | 8.7 | 29.5 | 161.4 | 3,504 ms | 2 | 1 | 0 |

Per-request p99/p99.9 are higher than these per-second means. Not computable from this source.

Bad period: 2026-09-07 15:43:14-15:43:30 UTC.
- 15:43:15-15:43:19: S3 requests nearly stop (0-4 per second).
- 15:43:20: one write took 10.67 s; nine reads totalled 31.5 s (3.5 s each).
- 15:43:30: 3 writes averaged 1.4 s, 8 reads averaged 1.1 s.
- Server log volume fell from about 400 lines/s to 30-95 lines/s for 8 s. This looks like a host or process stall (ASan CI runner under load), not a store-only stall. A single request over 5 s would have exceeded both 3 s and 5 s.

Timeouts in the server log (`Failed to make request to: http://localhost:11121/...: will be retried, Poco::TimeoutException`):
- 1,145 timeouts of about 6.38M S3 requests (0.018%). 1,136 are LIST requests (`list-type=2&prefix=cas_s3/cas/ns/stream/`), 9 are other keys (4 on `cas/ref_catalog`, 1 `_log` object, 4 blobs). 17 further retries were `Connection refused` (code 111).
- Clustered at 14:5x, 15:2x-15:3x, 16:2x-16:4x and 18:3x-18:4x (up to 31 per minute).
- The timeout value that fired is not recorded in the log or the CI config I read; do not treat these as 3 s or 5 s events.

CAS conditional writes in this job: 1,211,645 attempts, 1,208,033 committed, 3,611 unresolved (0.30%), 0 definite failures. "Unresolved" covers conflict, timeout, connection loss and server error together, so it is an upper bound on timeouts.

Queries (clickhouse-local on the decompressed TSV; `toUInt64OrZero(toString(...))` because schema inference types the columns as String):
```
zstd -dc tmp/cas223/ci0907/metric_log.tsv.zst > ci0907.tsv
SELECT sum(wu)/1e6, sum(wn), sum(we), sum(wt), sum(wa), sum(wr), ... FROM file('ci0907.tsv', TSVWithNames)
  -- wu = ProfileEvent_S3WriteMicroseconds, wn = ProfileEvent_S3WriteRequestsCount, we = ...RequestsErrors,
  -- wt = ...RequestsThrottling, wa = ...RequestAttempts, wr = ...RequestRetryableErrors; same for S3Read*
SELECT quantile(0.5)(wu/wn/1e3), quantile(0.9)(..), quantile(0.99)(..), quantile(0.999)(..), max(..),
       countIf(wu/wn>1e6), countIf(wu/wn>3e6), countIf(wu/wn>5e6) FROM t WHERE wn>0   -- also WHERE wn<=2; read: ru/rn
SELECT event_time, wu, wn, ru, rn ... WHERE event_time BETWEEN '2026-09-07 15:43:05' AND '2026-09-07 15:43:45'
grep -a "Failed to make request to: http://localhost:11121.*Timeout" ci0907.log     # after zstd -dc of the log
```

### Source 2: otel.demo stand, AWS S3, one week (saved audit, not live) {#q3-source-2-otel-demo-stand-aws-s3-one-week-saved-aud}

- Store: AWS S3 (audit prices requests at AWS list price), bucket `bvt-cas-test`, region not stated. Replica `chi-otel-otel-0-0`, 26.6.4.20001.altinityantalya, week ending 2026-09-25. Plain two-replica churn workload (~143k parts/day).
- Files: `docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md` and `.../2026-09-25-otel-demo-cas-s3-budget-audit/cas-profile-events-week.tsv`. I did not run queries; these are the audit's own `system.metric_log` / `system.events` figures.
- Control plane is visible through the CAS counters, but not as latency:

| counter (week) | value | meaning |
|---|---|---|
| `CASRequestAttempt` | 72,512,334 | all CAS control requests, incl. reissues |
| `CASConditionalWriteAttempts` / `Committed` | 14,791,894 / 14,784,276 | |
| `CASConditionalWriteUnresolved` | 7,616 (0.051%) | outcome unknown after conflict, timeout, connection loss, 5xx; each was settled by a read (`CASRequestResolveRead` 7,613). The audit calls this "the S3 timeout rate" |
| `CASRequestReissue` | 3,343 (0.0046% of attempts) | |
| `CASRequestFirstAttemptFuse` | 1,042 | fresh connection missed the adaptive first-attempt timeout; max 2 per minute |
| `CASRequestConnectFailureHint` | 3 | |
| `CASRequestRefused`, `CASRefAppendPreAttemptRefused` | 0 | |

- Per-request means (from phase rows): GC GETs about 20 ms per log (3.3 GETs per log); `pending_deletes` 44 ms per blob (3 serial requests); 5 S3 reads per `NewPart` cost 125 ms (25 ms each). The ref-lane flush is four serial round trips, about 249 ms wait for the lane per part. No percentile per request.
- Blob data requests (`blob_storage_log`): timeouts 2-5 per day, `internal error` about 1 per day. Those use the 30 s data client, not the 5 s control client.
- Throttling: 537 events in the week (`S3ReadRequestsThrottling` + `S3WriteRequestsThrottling`), concentrated in seconds with 7.6x LIST rate. Throttled PUTs were `_log`, `_ckpt`, manifests.
- Connections: 764,574 TLS connections created in the week, about 1 per 98 requests (F27), i.e. a handshake sits on the critical path 1.3 times per second per node. Handshake time not measured here.
- Share over 1, 3, 5 s: not available. p99.9: not available.

### Source 3: real GCS soak, per-phase GC rows (small samples) {#q3-source-3-real-gcs-soak-per-phase-gc-rows-small-sam}

- Store: GCS (`storage.googleapis.com`) reached from a workstation over the WAN, bucket `content-adressable-test-mfilimonov`. Files: `utils/ca-soak/soak_gcs_f11_gate_10m.db` (2026-09-03 ~10 min), `utils/ca-soak/soak_gcs_smoke_20260905.db`, table `gc_phases`, column `events_json` (ProfileEvents deltas per GC phase of the round thread only).
- Cannot separate control plane from data: per phase, round-thread requests only (mostly GETs of GC state, manifests, `gc/state`).

| phase (db) | requests | mean | notes |
|---|---|---|---|
| `lease` (f11 gate, 2 nodes) | 1,003 and 861 GET | 85 and 83 ms | 3 and 5 read errors; connect 83 and 76 ms mean over 134 and 130 new connections |
| `fold_reduce` (smoke) | 298 GET | 55 ms | 65 read errors (404-class) |
| `round_commit` (smoke) | 25 PUT, 25 GET | 146 ms PUT, 46 ms GET | |
| `manifest_deletes` (smoke) | 11 write | 221 ms | |
| `ref_object_cleanup` (smoke) | 2 write | 396 ms | |
| `lease` (smoke, other node) | 90 GET | 67 ms | 18 connects, 44 ms mean |

Means only; no p99. Phase `max_us` is the phase maximum, not a request maximum (largest 1.9 s for a whole `lease` phase).

Query:
```
sqlite3 -readonly utils/ca-soak/soak_gcs_f11_gate_10m.db "select ts,node,phase,total_us,max_us,events_json from gc_phases where events_json like '%S3%Micro%'"
  -- then json: S3WriteMicroseconds/S3WriteRequestsCount, S3ReadMicroseconds/S3ReadRequestsCount,
  --            DiskConnectionsElapsedMicroseconds/DiskConnectionsCreated
```

### Source 4: GCS soak write-ups (saved findings, bad moments) {#q3-source-4-gcs-soak-write-ups-saved-findings-bad-mom}

Files: `tmp/GCS-soak-report.md` (15 min phase 3, 2026-09-04), `docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md` (F3, F8, F9, F10).

- 2026-09-04 11:06-11:09 UTC: DNS resolution outage to `storage.googleapis.com` plus `Poco::TimeoutException: SSL_read timed out` retries on both nodes (40 timeout matches). `watermark_renew` failed on both nodes with `elapsed_ms` 11,800-12,000 (`external_lease_deadline`). Both lost mount authority for about 106 s and remounted at 11:08:48. Lasted about 2 minutes. A 5 s or 3 s per-request timeout makes no difference in a DNS outage.
- 2026-09-02 01:12-01:22 UTC (F10): ten-minute storm of TCP connect timeouts to every Google front-end IP the client resolved (500-2,900 log lines per minute on ch1, about 35k lines total). A 0.5 s connect cap would have hit the same storm, but the connect timeout value in force then was larger, so the count of lost attempts is unknown.
- 2026-09-02 01:15 UTC (F9): two TCP connect timeouts within 13 s; one on a `gc/gen/1/attempt/1/blob_target/0/0` PUT, one on mount-lease renewal (retried, lease kept with 19 s of budget left).
- F3: GCS `429 SlowDown` on one hot object per node (the mount `_ckpt`; 269 and 340 hits in the 15 min run). A throttled conditional write is not retried (per F3 note). Not a latency measure.
- F8 (data reads, not control plane): non-system SELECTs on ch1, n=537, p50 3.6 s, p99 15.7 s, max 30.8 s, about 725 GETs per SELECT. Per-query, so it says nothing about a single control request.

### What to collect next {#q3-what-to-collect-next}

The decision needs per-request control-plane latency: p99, p99.9 and max, with attempt number, over at least 7 days, on at least AWS S3 and GCS (the stands already exist), plus one MinIO/RustFS run. Options, cheapest first:

1. otel.demo (AWS S3), live, read-only, once Grafana or the native port is reachable:
   - `system.histogram_metrics` / Prometheus `S3FirstByte` and `DiskS3FirstByte` (labels: method, attempt; names in `src/Common/HistogramMetrics.cpp:52,59`, emitted in `src/IO/S3/PocoHTTPClient.cpp:405,410`). Take the PUT and GET buckets with attempt label `1`, and the share above 1, 3, 5 s from the bucket counts. This is first-byte time of all requests of that client, so it must be taken from the CAS control-plane disk client, or the data share has to be subtracted by request count. Check first whether the control client reports into these families.
   - Delta of `CASRequestFirstAttemptFuse`, `CASRequestReissue`, `CASConditionalWriteUnresolved` per day over 7+ days, with `metric_log` kept at 1 s.
2. Add one counter family to measure what the option needs: a histogram of control-request duration (start of send to last byte of response), per method and attempt, with buckets 0.1, 0.25, 0.5, 1, 2, 3, 5, 10 s, plus a connect-time histogram (buckets 0.05, 0.1, 0.25, 0.5, 1 s). Without it the question can only be bounded, not answered. Ship it to otel.demo and one GCS soak for a week.
3. Make soak server logs readable by the user (they are `syslog:syslog 0640`), or dump `system.text_log`, `system.metric_log` and `system.cas_log` for the 1 s window into `utils/ca-soak/logs/` at the end of each scenario. `cas_log` should carry the first `watermark_renew` (successful too) with `elapsed_ms` and `attempts_sent`.
4. For the 3 s test itself: run a soak or a CI shard with `cas_attempt_timeout_ms = 3000` and the 0.5 s connect, and read `CASRequestFirstAttemptFuse`, `CASRequestReissue`, `CASConditionalWriteUnresolved` against the 5 s baseline from item 1. This answers the question directly, on a stand where a false timeout costs nothing.

