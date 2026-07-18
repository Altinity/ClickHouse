# S22 throttle-retry FAIL — root-cause analysis (2026-07-18)

**Run:** `utils/ca-soak/scenarios/runs/20260718T000307_S22_seed1/`
**Scenario:** S22 "object-store throttling and retry budget", scale `ci`, variant `s3faultproxy`
**Branch:** `cas-gc-rebuild` @ `4420b5a3498b` (dirty)
**Verdict:** **(a) product bug** — the freshness-meta sidecar conditional write bypasses the CAS retry/budget controller, so an injected `SlowDown` on that single PUT escapes unretried as a hard client error.

---

## 1. What the scenario injects and expects

An HTTP fault proxy (`docker-compose-s3faultproxy.yml`, control port `:8474`) sits between ClickHouse
and RustFS. With faults armed at `rate=0.2` over modes `["503","429","slow"]` on
`GET/PUT/HEAD/POST`, S22 runs an insert + `OPTIMIZE ... FINAL` merge workload across 4 tables and
asserts the workload **completes with 0 hard errors** (the S3 client must retry transient faults
within its budget), that faults were actually injected (non-vacuous), and that replicas converge with
`fsck dangling==0`.

Result: 195 faults injected, replicas agreed, fsck clean, retries were bounded — **but 2 statements
reached the client as `HTTP 500: Code: 499 (S3_ERROR) ... Message: Please reduce your request rate.,
key soak_pool/blobs/ch128/<xx>/<hash>.meta, object size 71`**. `Please reduce your request rate` is
the S3 `SlowDown` body; `object size 71` and the `.meta` suffix identify these as CAS **blob
freshness-meta sidecars** (`CasLayout::blobMetaKey` = `blobKey` + `.meta`), not blob bodies.

## 2. The code path (why it is not retried)

The blob **body** upload is fault-tolerant: it runs through
`Pool::stagingConditionalCreate` → `CasRequestController::conditionalCreateControlled`
(`CasPartWriteTxn.cpp:527`, `CasRequestControl.cpp:387`). There a `SlowDown` is classified
`Unresolved` by `classifyConditionalWriteResult` (`CasRequestControl.cpp:82-101` — only
Malformed/EntityTooLarge/AccessDenied are `DefiniteFailure`; everything else, including any 5xx/429,
falls through to `Unresolved`), then resolved-by-HEAD and **reissued within the budget**
(`attempt_timeout_ms=5000`, `operation_deadline_ms=90000`, `max_attempts=16`, capped-exponential
backoff — `CasRequestControl.h:71-105`). This is why 193 of 195 faults were absorbed.

The freshness-meta sidecar write is **not** on that path. Immediately after the body commits,
`uploadFromSource` calls `writeFreshMetaClean()` (`CasPartWriteTxn.cpp:554`/`574`), and the
adopt/resurrect paths call `writeResurrectMetaClean` / the adopt backfill
(`CasPartWriteTxn.cpp:345`, `446`, `465-466`). All of these go straight to
`putMetaIfAbsent` / `casMeta` in `CasBlobMeta.cpp:24-36`, which call **`backend.casPut(...)` directly**
— no controller, no budget loop.

`ObjectStorageBackend::casPut` (`CasObjectStorageBackend.cpp:770`) uses `conditionalWriteSettings()`,
which forces `s3_max_unexpected_write_error_retries_override = 1`
(`CasObjectStorageBackend.cpp:700`) — a **single** HTTP attempt at the SDK level. The `SlowDown`
surfaces from `detail::finalizeConditionalWrite` (`CasObjectStorageBackend.cpp:207-222`), which maps
only `PreconditionFailed`/`NoSuchKey` and **rethrows everything else**. So the throw is an
`S3Exception` with code `S3_ERROR` (499).

Nothing above catches it: `writeResurrectMetaClean`'s bounded loop
(`CasPartWriteTxn.cpp:461-469`) only retries a `CasOutcome::Conflict`, not a thrown exception; and
`putBlob`'s bounded loop (`CasPartWriteTxn.cpp:204-224`) rethrows anything whose code
`!= ABORTED`. The `S3_ERROR` therefore propagates out of the INSERT/merge to the client as HTTP 500.
The net retry count for the meta sidecar is **zero** (single SDK attempt × no controller), so at
`fault_rate=0.2` each meta PUT has a flat ~20% chance of a hard failure — exactly the low-frequency
"2 escaped out of 195" signature observed.

## 3. Why not (b) or (c)

- Not **(b) budget exhausted**: the escaping error is `S3_ERROR (499)`, i.e. a *single* thrown
  `SlowDown`, not the controller's budget-exhaustion signal (`NETWORK_ERROR` "CAS write could not be
  committed … retrying later" from `throwCasWriteRetryLater`). The fault window (~90 s workload) also
  sits within the 90 s `operation_deadline_ms`, and body PUTs on the controller path succeeded
  throughout the same window.
- Not **(c) scenario mis-calibration**: the contract (0 hard errors under transient, retryable faults)
  is correct and is the guarantee the controller already delivers for every other conditional write.
  The meta sidecar is the sole conditional-write class on the INSERT/merge hot path that was never
  wired to it.

## 4. Fix direction

Route the freshness-meta conditional writes through the `CasRequestController` so `SlowDown`/`429`/5xx
are `Unresolved`→resolve-and-reissue within budget, identical to the body path:

- Give `CasBlobMeta.cpp` (`putMetaIfAbsent` / `casMeta`, and consistently `deleteMetaExact`) access to
  the pool's shared controller instead of calling `backend.casPut` directly — e.g. `putMetaIfAbsent`
  via `putIfAbsentControlled` (byte-exact `.meta` content makes retry safe), and the If-Match
  `casMeta` via a controlled put-overwrite variant. The meta bytes are small and deterministic, so
  the controller's exact-key resolution applies cleanly.
- The already-existing `writeResurrectMetaClean` Conflict loop should remain, but the transient
  transport error must be absorbed *inside* the controlled call rather than thrown past it.
- Do **not** paper over it by swallowing the error on the "best-effort" comment: a dropped meta write
  leaves stale freshness state for the next point-reader; the correct behavior is a budgeted retry,
  not a silent skip.

The corresponding regression check is S22 itself (meta-PUT faults must be absorbed); a unit-level
gate mirroring `CasPartWriteTxn.PutBlobWrongSizeFailsClosed` — arm a backend that throws `SlowDown` on
the `.meta` key and assert the meta write survives within budget — would pin it deterministically.
